// PartyUp 服务端单二进制入口。
//
// 子命令：
//
//	partyup serve  [--data DIR] [--http ADDR] [--name NAME] [--log-level L]
//	               [--session-ttl DUR] [--login-rate-limit N/WINDOW] [--trust-proxy]  启动服务
//	partyup invite [--data DIR] [--by MEMBER_ID] [--server-url URL]          另发一个邀请码
//	partyup sessions [--data DIR] [--active]                                列出会话（运维核对用）
//	partyup users  [--data DIR]                                             列出成员与角色（运维核对用）
//	partyup version                                                          打印版本
//
// 输出约定：stdout 只放机器可读的行（KEY=VALUE 与 "READY"），日志全部走 stderr。
// 这样一键复现脚本可以直接从 stdout 取 BOOTSTRAP_TOKEN / INVITE_CODE / LISTEN_ADDR，
// 不需要解析日志，也不会被日志格式变化打断。
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"partyup/internal/auth"
	"partyup/internal/invite"
	"partyup/internal/protocol"
	"partyup/internal/server"
	"partyup/internal/sfu"
	"partyup/internal/store"
	"partyup/internal/stunserver"
)

// version 是二进制版本。协议版本在 protocol.Version（当前 1）。
//
// 必须是 var 而不是 const：scripts/package.ps1 用 `-ldflags -X main.version=<VERSION>` 注入，
// 版本号的唯一来源是仓库根的 VERSION 文件；直接 `go build`（不走打包脚本）时为 "dev"。
var version = "dev"

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "partyup:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	cmd := "serve"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		cmd = args[0]
		args = args[1:]
	}
	switch cmd {
	case "serve":
		return cmdServe(args)
	case "invite":
		return cmdInvite(args)
	case "sessions":
		return cmdSessions(args)
	case "users":
		return cmdUsers(args)
	case "version":
		fmt.Printf("partyup %s (protocol v%d)\n", version, protocol.Version)
		return nil
	case "help", "-h", "--help":
		printUsage()
		return nil
	default:
		printUsage()
		return fmt.Errorf("unknown command %q", cmd)
	}
}

func envOr(key, fallback string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return fallback
}

func printUsage() {
	fmt.Fprint(os.Stderr, `partyup — PartyUp 服务端

用法:
  partyup serve  [--data DIR] [--http ADDR] [--name NAME] [--log-level L]
                 [--voice-mode relay|direct] [--screen-mode relay|direct]
                 [--session-ttl DUR] [--login-rate-limit N/WINDOW] [--trust-proxy]
                 [--stun-listen ADDR] [--stun-advertise HOST:PORT]
                 [--ice-port-range MIN-MAX] [--ice-public-ip IP[,IP...]]
  partyup invite [--data DIR] [--by MEMBER_ID] [--server-url URL]
  partyup sessions [--data DIR] [--active]
  partyup users  [--data DIR]
  partyup version

serve 的 stdout 输出（每行 KEY=VALUE，最后一行 READY）:
  DATA_DIR / DB_PATH / OWNER_FALLBACK / BOOTSTRAP_TOKEN / INVITE_CODE /
  VOICE_MODE / SCREEN_MODE / SESSION_TTL / LOGIN_RATE_LIMIT / TRUST_PROXY / STUN /
  ICE_PORT_RANGE / ICE_PUBLIC_IP / LISTEN_ADDR（SCREEN_MODE=direct 时屏幕共享只支持 1v1）
OWNER_FALLBACK 只在旧库迁移把最早成员回填为 owner 时出现一次。
--trust-proxy 仅在反向代理已终止 TLS 时开启：bootstrap 默认只允许回环或 TLS 连接。
--session-ttl 接受 Go duration（如 720h=30 天）；--login-rate-limit 接受
<次数>/<窗口>（如 10/15m），传 0 关闭登录限速。
--ice-port-range 固定媒体 ICE 的 UDP 端口区间（MIN-MAX，单端口等价 MIN=MAX；
环境变量 PARTYUP_ICE_PORT_RANGE）。--ice-public-ip 把 host 候选改写成对外 IP
（逗号分隔；环境变量 PARTYUP_ICE_PUBLIC_IP）。两者配合 Docker bridge 的
-p 50000-50010:50000-50010/udp 即可跑媒体，不必 --network host；未设置时为
ICE_PORT_RANGE=unset / ICE_PUBLIC_IP=unset，行为与旧版本一致。

invite 的 stdout 输出:
  INVITE_CODE=<code>
  INVITE_LINK=<http(s) base URL>?code=<code>   （仅在显式给 --server-url 时）
--server-url 是管理员声明的对外服务器 base URL；程序不推断、不读取 Host/Forwarded，
非法 URL 会在创建邀请码之前被拒绝（不会留下有效邀请码）。
`)
}

func cmdServe(args []string) error {
	fs := flag.NewFlagSet("serve", flag.ContinueOnError)
	dataDir := fs.String("data", "./data", "数据目录（SQLite 库与未来附件都在这里）")
	httpAddr := fs.String("http", "127.0.0.1:8080", "HTTP 监听地址")
	name := fs.String("name", "PartyUp", "服务端展示名")
	logLevel := fs.String("log-level", "info", "日志级别：debug|info|warn|error")
	// 语音传输策略：客户端无权选择，由运维在这里定（默认中继，见 ADR-0002）。
	voiceMode := fs.String("voice-mode", envOr("PARTYUP_VOICE_MODE", "relay"),
		"语音传输策略：relay（服务端 SFU 中继，默认）| direct（P2P 直连）")
	// 屏幕共享是**独立**的传输策略，与语音互不影响（ADR-0002：三类媒体各自决定路径）。
	// 默认 relay（2026-11-04 起）：direct 只能 1v1，多于 2 人时第三个人看不到共享，
	// 默认配置必须是"多数人能看到"的。想省带宽的部署者显式传 --screen-mode=direct。
	screenMode := fs.String("screen-mode", envOr("PARTYUP_SCREEN_MODE", "relay"),
		"屏幕共享传输策略：relay（服务端 SFU 视频中继，默认）| direct（P2P 直连，仅支持 1v1）")
	// 会话有效期（2026-11-07，PROTOCOL.md §1.1）：Go duration，默认 30 天。
	// 采用滑动续期：WS 鉴权成功与 GET /api/me 命中会把过期时间往后推。
	sessionTTL := fs.String("session-ttl", envOr("PARTYUP_SESSION_TTL", "720h"),
		"会话有效期（Go duration，如 720h=30 天）；WS 鉴权与 /api/me 命中时滑动续期")
	// 登录失败限速（2026-11-07）：按 IP + 用户名计数，成功登录清零；0 关闭。
	loginRateLimit := fs.String("login-rate-limit", envOr("PARTYUP_LOGIN_RATE_LIMIT", "10/15m"),
		"登录失败限速 <次数>/<窗口>（如 10/15m）；0 关闭")
	// 内建 STUN responder（直连模式的 srflx 候选来源，见 docs/PROTOCOL.md §1.4）。
	// 默认监听 :3478；传空串 / off 关闭。advertise 为空时自动从 listen 推导，
	// 但**多网卡或有 NAT 的机器必须显式指定**，否则通告的是内网地址。
	stunListen := fs.String("stun-listen", envOr("PARTYUP_STUN_LISTEN", ":3478"),
		"STUN UDP 监听地址，默认 :3478；传空 / off 关闭")
	stunAdvertise := fs.String("stun-advertise", envOr("PARTYUP_STUN_ADVERTISE", ""),
		"STUN 对外通告的 IP:port（多网卡/NAT 必填；空则从 listen 或本机网卡推导）")
	// 媒体 ICE 的端口范围与对外 IP（2026-11-08，task-6）：Docker bridge 部署靠
	// 固定 UDP 区间 + host 候选改写，把媒体端口映射出来即可，不必 --network host。
	// 空 = 保持旧行为（系统随机端口、不改写候选）。
	icePortRange := fs.String("ice-port-range", envOr("PARTYUP_ICE_PORT_RANGE", ""),
		"媒体 ICE 的 UDP 端口区间 MIN-MAX（如 50000-50060；单端口 50000 等价 50000-50000）；空=不限制")
	icePublicIP := fs.String("ice-public-ip", envOr("PARTYUP_ICE_PUBLIC_IP", ""),
		"媒体 host 候选改写成的对外 IP（逗号分隔；VPS 填公网 IP，家用 NAT 填公网 IP 并做端口转发）；空=不改写")
	// bootstrap 安全边界（2026-11-07，规格 §7.4）：默认只允许回环或 TLS 连接执行
	// bootstrap；部署在反向代理后（代理已终止 TLS）时才显式开启。
	trustProxy := fs.Bool("trust-proxy", false,
		"信任 X-Forwarded-For 并允许代理后的 bootstrap；仅在反向代理已终止 TLS 时开启")
	if err := fs.Parse(args); err != nil {
		return err
	}

	// 配置在打开库/监听端口之前校验：非法值直接拒绝启动，避免「以为开了限速
	// 或过期，实际没生效」这种只在事故里才发现的静默失败。
	ttl, err := time.ParseDuration(*sessionTTL)
	if err != nil {
		return fmt.Errorf("--session-ttl: %w", err)
	}
	if ttl <= 0 {
		return fmt.Errorf("--session-ttl must be > 0, got %q", *sessionTTL)
	}
	rateLimit, err := server.ParseLoginRateLimit(*loginRateLimit)
	if err != nil {
		return err
	}
	// 媒体 ICE 配置也必须在使用前校验：非法端口区间/IP 直接拒绝启动，
	// 避免「以为映射了固定端口，实际 Pion 用了随机端口」这种事故。
	ice, err := sfu.ParseICEConfig(*icePortRange, *icePublicIP)
	if err != nil {
		return err
	}

	logger := newLogger(*logLevel)

	st, err := store.Open(*dataDir)
	if err != nil {
		return err
	}
	defer st.Close()

	// 角色迁移（2026-11-07，规格 §1）：旧库没有 owner 时把最早创建的成员回填为
	// owner，并建「至多一个 owner」的唯一索引。必须在任何请求之前完成，否则
	// 权限判断会看到一堆 role=user 的老成员（管理员位只认 role）。
	ownerCtx, ownerCancel := context.WithTimeout(context.Background(), 10*time.Second)
	ownerFallback, err := st.EnsureOwner(ownerCtx)
	ownerCancel()
	if err != nil {
		return fmt.Errorf("owner migration: %w", err)
	}

	stunSrv, err := stunserver.Start(*stunListen, *stunAdvertise, logger)
	if err != nil {
		return fmt.Errorf("stun responder: %w", err)
	}
	defer func() { _ = stunSrv.Close() }()
	var stunURLs []string
	if u := stunSrv.URL(); u != "" {
		stunURLs = []string{u}
	}

	srv := server.New(st, server.Options{
		Name:           *name,
		Version:        version,
		Logger:         logger,
		VoiceMode:      *voiceMode,
		ScreenMode:     *screenMode,
		StunURLs:       stunURLs,
		SessionTTL:     ttl,
		LoginRateLimit: rateLimit,
		TrustProxy:     *trustProxy,
		ICE:            ice,
	})

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	info, err := srv.EnsureFirstRun(ctx)
	cancel()
	if err != nil {
		return fmt.Errorf("first-run setup: %w", err)
	}

	absData, err := filepath.Abs(*dataDir)
	if err != nil {
		absData = *dataDir
	}

	// 先 bind 再打印：脚本看到 LISTEN_ADDR 就能立刻连，不存在「打印了但还没监听」的窗口。
	ln, err := net.Listen("tcp", *httpAddr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", *httpAddr, err)
	}

	fmt.Printf("partyup %s\n", version)
	fmt.Printf("PROTOCOL_VERSION=%d\n", protocol.Version)
	fmt.Printf("DATA_DIR=%s\n", absData)
	fmt.Printf("DB_PATH=%s\n", st.Path())
	if ownerFallback != "" {
		// 旧库升级时把最早成员回填为 owner 的提示（规格 §1）；只出现一次。
		fmt.Printf("OWNER_FALLBACK=%s\n", ownerFallback)
	}
	if info.Token != "" {
		fmt.Printf("BOOTSTRAP_TOKEN=%s\n", info.Token)
	}
	if info.InviteCode != "" {
		fmt.Printf("INVITE_CODE=%s\n", info.InviteCode)
	}
	fmt.Printf("VOICE_MODE=%s\n", server.NormalizeVoiceMode(*voiceMode))
	fmt.Printf("SCREEN_MODE=%s\n", server.NormalizeScreenMode(*screenMode))
	fmt.Printf("SESSION_TTL=%s\n", ttl)
	fmt.Printf("LOGIN_RATE_LIMIT=%s\n", rateLimit)
	fmt.Printf("TRUST_PROXY=%t\n", *trustProxy)
	// 媒体 ICE 生效配置（2026-11-08）：验收脚本/排障靠这两行确认固定端口与对外 IP
	// 真的传到了进程里，而不是只写在 compose 文件里没生效。
	fmt.Printf("ICE_PORT_RANGE=%s\n", ice.PortRangeString())
	fmt.Printf("ICE_PUBLIC_IP=%s\n", ice.PublicIPsString())
	if adv := stunSrv.Advertise(); adv != "" {
		fmt.Printf("STUN=udp://%s\n", adv)
	} else {
		fmt.Println("STUN=off")
	}
	fmt.Printf("LISTEN_ADDR=%s\n", ln.Addr().String())
	fmt.Println("READY")

	httpSrv := &http.Server{
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       120 * time.Second,
	}
	serveErr := make(chan error, 1)
	go func() { serveErr <- httpSrv.Serve(ln) }()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sigCh)

	logger.Info("partyup server listening", "addr", ln.Addr().String(), "db", st.Path())

	select {
	case err := <-serveErr:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	case sig := <-sigCh:
		logger.Info("shutting down", "signal", sig.String())
	}

	// 先断 WS（否则 Shutdown 会等空闲连接直到超时），再停 HTTP。
	srv.Hub().Close()
	shutdownCtx, cancelShutdown := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancelShutdown()
	if err := httpSrv.Shutdown(shutdownCtx); err != nil {
		logger.Warn("http shutdown", "err", err)
	}
	return nil
}

func cmdInvite(args []string) error {
	fs := flag.NewFlagSet("invite", flag.ContinueOnError)
	dataDir := fs.String("data", "./data", "数据目录")
	createdBy := fs.String("by", "", "创建者成员 id（可选，仅记录用）")
	serverURL := fs.String("server-url", "",
		"可选：管理员声明的对外服务器 base URL；给出时额外输出 INVITE_LINK")
	if err := fs.Parse(args); err != nil {
		return err
	}

	// URL 校验先于创建邀请码：非法输入不允许留下任何可用邀请码。
	// 不推断外部地址，也不读取 Host/Forwarded 头——管理员显式声明才生成链接。
	var base invite.ServerBase
	withLink := *serverURL != ""
	if withLink {
		var err error
		base, err = invite.ParseServerBase(*serverURL)
		if err != nil {
			return err
		}
	}

	st, err := store.Open(*dataDir)
	if err != nil {
		return err
	}
	defer st.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	for attempt := 0; attempt < 5; attempt++ {
		code, err := auth.NewInviteCode()
		if err != nil {
			return err
		}
		if _, err := st.CreateInvite(ctx, code, *createdBy); errors.Is(err, store.ErrConflict) {
			continue
		} else if err != nil {
			return err
		}
		fmt.Printf("INVITE_CODE=%s\n", code)
		if withLink {
			link, err := base.Link(code)
			if err != nil {
				return err
			}
			fmt.Printf("INVITE_LINK=%s\n", link)
		}
		return nil
	}
	return errors.New("could not allocate a unique invite code")
}

// cmdSessions 列出库里的会话（运维/验收用）。
//
// 为什么需要它：单端登录的正确性要能在服务端侧直接核对——「同一账号只剩一条
// active 会话、旧会话 revoked_at 非空」。SQLite 是单文件库，但客户端机器通常
// 没有 sqlite3 命令行；本子命令只读、不写库，输出机器可读的 KEY=VALUE 行。
// 只打印 token 哈希前缀（12 位 hex），不打印 token 原文——库里本来也没有原文。
func cmdSessions(args []string) error {
	fs := flag.NewFlagSet("sessions", flag.ContinueOnError)
	dataDir := fs.String("data", "./data", "数据目录")
	activeOnly := fs.Bool("active", false, "只列出仍有效的会话（未撤销且未过期）")
	if err := fs.Parse(args); err != nil {
		return err
	}

	st, err := store.Open(*dataDir)
	if err != nil {
		return err
	}
	defer st.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	sessions, err := st.ListSessions(ctx)
	if err != nil {
		return err
	}

	shown := 0
	for _, s := range sessions {
		if *activeOnly && !s.Active {
			continue
		}
		shown++
	}
	fmt.Printf("SESSIONS=%d\n", shown)
	for _, s := range sessions {
		if *activeOnly && !s.Active {
			continue
		}
		fmt.Printf("SESSION member=%s member_id=%s hash_prefix=%s created_at=%d expires_at=%d revoked_at=%d active=%t\n",
			s.MemberName, s.MemberID, s.TokenHashPrefix, s.CreatedAt, s.ExpiresAt, s.RevokedAt, s.Active)
	}
	return nil
}

// cmdUsers 列出库里的成员与角色（运维/验收用）。
//
// 为什么需要它：owner 唯一性、旧库回填结果、封禁状态都要能在服务端侧直接核对，
// 而客户端机器通常没有 sqlite3。本子命令只读、不写库，输出机器可读的 KEY=VALUE 行。
func cmdUsers(args []string) error {
	fs := flag.NewFlagSet("users", flag.ContinueOnError)
	dataDir := fs.String("data", "./data", "数据目录")
	if err := fs.Parse(args); err != nil {
		return err
	}

	st, err := store.Open(*dataDir)
	if err != nil {
		return err
	}
	defer st.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	// EnsureOwner 在这里也调用：只读核对希望看到迁移后的真实状态，而不是
	// 「列还没建/owner 还没回填」的中间态。
	if _, err := st.EnsureOwner(ctx); err != nil {
		return err
	}
	members, err := st.ListMembersAdmin(ctx)
	if err != nil {
		return err
	}
	owners := 0
	for _, m := range members {
		if m.Role == store.RoleOwner {
			owners++
		}
	}
	fmt.Printf("USERS=%d\n", len(members))
	fmt.Printf("OWNERS=%d\n", owners)
	for _, m := range members {
		fmt.Printf("USER member=%s member_id=%s role=%s admin=%t banned=%t created_at=%d\n",
			m.Name, m.ID, m.Role, m.Admin, m.Banned, m.CreatedAt)
	}
	return nil
}

// newLogger 输出到 stderr：stdout 留给机器可读的启动块。
func newLogger(level string) *slog.Logger {
	var lvl slog.Level
	switch strings.ToLower(level) {
	case "debug":
		lvl = slog.LevelDebug
	case "warn":
		lvl = slog.LevelWarn
	case "error":
		lvl = slog.LevelError
	default:
		lvl = slog.LevelInfo
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: lvl}))
}
