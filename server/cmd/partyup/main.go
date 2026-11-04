// PartyUp 服务端单二进制入口。
//
// 子命令：
//
//	partyup serve  [--data DIR] [--http ADDR] [--name NAME] [--log-level L]   启动服务
//	partyup invite [--data DIR] [--by MEMBER_ID]                             另发一个邀请码
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
	"partyup/internal/protocol"
	"partyup/internal/server"
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
                 [--stun-listen ADDR] [--stun-advertise HOST:PORT]
  partyup invite [--data DIR] [--by MEMBER_ID]
  partyup version

serve 的 stdout 输出（每行 KEY=VALUE，最后一行 READY）:
  DATA_DIR / DB_PATH / BOOTSTRAP_TOKEN / INVITE_CODE / VOICE_MODE / SCREEN_MODE / STUN /
  LISTEN_ADDR（SCREEN_MODE=direct 时屏幕共享只支持 1v1）
BOOTSTRAP_TOKEN 与 INVITE_CODE 只在本次启动新生成时出现。
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
	// 内建 STUN responder（直连模式的 srflx 候选来源，见 docs/PROTOCOL.md §1.4）。
	// 默认监听 :3478；传空串 / off 关闭。advertise 为空时自动从 listen 推导，
	// 但**多网卡或有 NAT 的机器必须显式指定**，否则通告的是内网地址。
	stunListen := fs.String("stun-listen", envOr("PARTYUP_STUN_LISTEN", ":3478"),
		"STUN UDP 监听地址，默认 :3478；传空 / off 关闭")
	stunAdvertise := fs.String("stun-advertise", envOr("PARTYUP_STUN_ADVERTISE", ""),
		"STUN 对外通告的 IP:port（多网卡/NAT 必填；空则从 listen 或本机网卡推导）")
	if err := fs.Parse(args); err != nil {
		return err
	}

	logger := newLogger(*logLevel)

	st, err := store.Open(*dataDir)
	if err != nil {
		return err
	}
	defer st.Close()

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
		Name:       *name,
		Version:    version,
		Logger:     logger,
		VoiceMode:  *voiceMode,
		ScreenMode: *screenMode,
		StunURLs:   stunURLs,
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
	if info.Token != "" {
		fmt.Printf("BOOTSTRAP_TOKEN=%s\n", info.Token)
	}
	if info.InviteCode != "" {
		fmt.Printf("INVITE_CODE=%s\n", info.InviteCode)
	}
	fmt.Printf("VOICE_MODE=%s\n", server.NormalizeVoiceMode(*voiceMode))
	fmt.Printf("SCREEN_MODE=%s\n", server.NormalizeScreenMode(*screenMode))
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
		return nil
	}
	return errors.New("could not allocate a unique invite code")
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
