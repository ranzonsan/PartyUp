// 登录失败限速（2026-11-07，PROTOCOL.md §1.1）。
//
// 防御目标：密码猜测。argon2id 很贵，所以限速判定必须在**任何密码校验之前**做，
// 否则攻击者靠并发请求就能把 CPU 打满（这本身就是一种 DoS）。
//
// 维度是「IP + 用户名」：只按 IP 会让 NAT 后的室友互相误伤；只按用户名等于给
// 攻击者「换个用户名继续试」的自由。两者拼成一个键取交集。
//
// 状态纯进程内（map），重启归零——v1 是单实例自托管，不需要跨进程一致性；为它
// 引入 Redis 之类的外部依赖与「单二进制、无外部服务」的项目取舍冲突。
//
// **不锁定已有 token 的连接**：限速只作用在 POST /api/login 的尝试上；已签发的
// 会话与在线 WS 完全不受影响，别人试密码不会把在线用户踢下线。
package server

import (
	"fmt"
	"math"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

// LoginRateLimit 是登录失败限速的配置。MaxFailures 或 Window 非正表示关闭。
type LoginRateLimit struct {
	MaxFailures int           // 窗口内允许的失败尝试数（桶容量）
	Window      time.Duration // 从空桶回满所需的时间
}

// Enabled 报告是否启用限速。
func (c LoginRateLimit) Enabled() bool { return c.MaxFailures > 0 && c.Window > 0 }

// String 返回稳定的描述串（启动块输出与日志用；关闭时是 "off"）。
func (c LoginRateLimit) String() string {
	if !c.Enabled() {
		return "off"
	}
	return fmt.Sprintf("%d/%s", c.MaxFailures, c.Window)
}

// ParseLoginRateLimit 解析 CLI/环境变量里的 `N/窗口` 规格，例如 "10/15m"。
// 空串、"0"、"off"、"none" 表示关闭（返回零值 LoginRateLimit）。
// 非法输入返回 error，由启动流程拒绝启动——不要让运维以为自己开了限速。
func ParseLoginRateLimit(spec string) (LoginRateLimit, error) {
	s := strings.TrimSpace(strings.ToLower(spec))
	switch s {
	case "", "0", "off", "none", "disabled":
		return LoginRateLimit{}, nil
	}
	parts := strings.SplitN(s, "/", 2)
	if len(parts) != 2 {
		return LoginRateLimit{}, fmt.Errorf("login rate limit: want <count>/<duration> (e.g. 10/15m), got %q", spec)
	}
	n, err := strconv.Atoi(strings.TrimSpace(parts[0]))
	if err != nil || n <= 0 {
		return LoginRateLimit{}, fmt.Errorf("login rate limit: invalid failure count in %q", spec)
	}
	window, err := time.ParseDuration(strings.TrimSpace(parts[1]))
	if err != nil {
		return LoginRateLimit{}, fmt.Errorf("login rate limit: invalid window in %q: %w", spec, err)
	}
	if window <= 0 {
		return LoginRateLimit{}, fmt.Errorf("login rate limit: window must be > 0 in %q", spec)
	}
	return LoginRateLimit{MaxFailures: n, Window: window}, nil
}

// loginLimiter 是进程内的 token bucket 集合。
//
// 每个键一只桶：容量 = MaxFailures，按 Window 匀速回满（速率 = 容量 / Window）。
// 稳态语义等价于「Window 内最多 MaxFailures 次失败」，但突发后能平滑恢复
// （空桶后每 Window/容量 就能再试一次），比固定窗口实现更简单也更友好。
//
// 用「先扣令牌、成功再清零」而不是「失败才记账」：并发攻击时前者能精确地只
// 放行容量条尝试；后者会让 N 个并发请求先全部通过检查、再一起记失败
// （实际放行次数远超配置）。
type loginLimiter struct {
	mu       sync.Mutex
	capacity float64       // 桶容量（= MaxFailures）
	window   time.Duration // 从空桶回满的时间
	buckets  map[string]*loginBucket
	now      func() time.Time // 测试注入；生产用 time.Now
	lastGC   time.Time
}

type loginBucket struct {
	tokens float64   // 当前可用令牌（0..capacity）
	last   time.Time // 上次结算时刻
}

func newLoginLimiter(cfg LoginRateLimit) *loginLimiter {
	return &loginLimiter{
		capacity: float64(cfg.MaxFailures),
		window:   cfg.Window,
		buckets:  make(map[string]*loginBucket),
		now:      time.Now,
	}
}

// loginRateKey 把 IP 与用户名拼成限速键。用户名按不区分大小写归一（库里 name
// 是 COLLATE NOCASE），否则攻击者换个大小写就换到一个新桶。
func loginRateKey(ip, username string) string {
	return ip + "\x00" + strings.ToLower(strings.TrimSpace(username))
}

// allow 尝试占用一次尝试额度。ok=false 时 retryAfter 是「还要多久才有一次机会」。
// 调用方在登录成功时必须 reset；失败则什么都不用做（额度已在 allow 里扣掉）。
func (l *loginLimiter) allow(key string) (ok bool, retryAfter time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()

	now := l.now()
	l.gcLocked(now)

	// 速率以「每毫秒令牌数」表示。capacity>=1、window>=1ms 保证 >0。
	ratePerMS := l.capacity / float64(l.window.Milliseconds())

	b, exists := l.buckets[key]
	if !exists {
		b = &loginBucket{tokens: l.capacity, last: now}
		l.buckets[key] = b
	}
	elapsedMS := float64(now.Sub(b.last).Milliseconds())
	if elapsedMS > 0 {
		b.tokens = math.Min(l.capacity, b.tokens+elapsedMS*ratePerMS)
		b.last = now
	}
	if b.tokens < 1 {
		waitMS := int64(math.Ceil((1 - b.tokens) / ratePerMS))
		if waitMS < 1 {
			waitMS = 1
		}
		return false, time.Duration(waitMS) * time.Millisecond
	}
	b.tokens--
	return true, 0
}

// reset 在登录成功后清空该键：成功登录即清零，避免合法用户被自己此前的手误
// 拖累（PROTOCOL.md §1.1）。
func (l *loginLimiter) reset(key string) {
	l.mu.Lock()
	delete(l.buckets, key)
	l.mu.Unlock()
}

// gcLocked 定期清理长期不用的桶，防止 map 被随机用户名/IP 撑爆（进程内内存也
// 是攻击面）。节流：每个 Window 最多扫一次；扫描 O(桶数)，登录路径可接受。
//
// 一个桶在空闲一个 Window 后必然回满（回满速率定义如此），所以「上次结算距今
// >= Window」就是安全的删除条件。
func (l *loginLimiter) gcLocked(now time.Time) {
	if !l.lastGC.IsZero() && now.Sub(l.lastGC) < l.window {
		return
	}
	l.lastGC = now
	for k, b := range l.buckets {
		if now.Sub(b.last) >= l.window {
			delete(l.buckets, k)
		}
	}
}

// clientIP 取 TCP 对端 IP（不含端口）。
//
// 刻意**不读** X-Forwarded-For / X-Real-IP：这些头由客户端随意伪造，在没有
// 配置可信反代时会变成「换个头就绕过限速」。v1 自托管默认直连；代理场景下
// 由运维在反代层做限速更合适。
func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}
