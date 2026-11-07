package server

// 登录限速的单元回归：解析规格、token bucket 的放行/回填/清零、桶清理。
// 这些是不需要 HTTP 的纯逻辑，用注入时钟做确定性断言（不 sleep）。

import (
	"testing"
	"time"
)

func TestParseLoginRateLimit(t *testing.T) {
	cases := []struct {
		spec    string
		want    LoginRateLimit
		wantErr bool
	}{
		{spec: "10/15m", want: LoginRateLimit{MaxFailures: 10, Window: 15 * time.Minute}},
		{spec: " 3/1m ", want: LoginRateLimit{MaxFailures: 3, Window: time.Minute}},
		{spec: "1/30s", want: LoginRateLimit{MaxFailures: 1, Window: 30 * time.Second}},
		// 关闭。
		{spec: "", want: LoginRateLimit{}},
		{spec: "0", want: LoginRateLimit{}},
		{spec: "off", want: LoginRateLimit{}},
		{spec: "none", want: LoginRateLimit{}},
		// 非法：启动时拒绝，不允许静默降级成「不限速」。
		{spec: "10", wantErr: true},        // 缺窗口
		{spec: "0/15m", wantErr: true},     // 次数必须 > 0
		{spec: "10/0s", wantErr: true},     // 窗口必须 > 0
		{spec: "-1/15m", wantErr: true},    // 负数
		{spec: "abc/15m", wantErr: true},   // 次数不是数字
		{spec: "10/nope", wantErr: true},   // 窗口不是 duration
		{spec: "10/15m/1h", wantErr: true}, // 多余的分段
	}
	for _, tc := range cases {
		got, err := ParseLoginRateLimit(tc.spec)
		if tc.wantErr {
			if err == nil {
				t.Errorf("ParseLoginRateLimit(%q) err = nil, want error", tc.spec)
			}
			continue
		}
		if err != nil {
			t.Errorf("ParseLoginRateLimit(%q): %v", tc.spec, err)
			continue
		}
		if got != tc.want {
			t.Errorf("ParseLoginRateLimit(%q) = %+v, want %+v", tc.spec, got, tc.want)
		}
	}
}

func TestLoginRateLimitEnabledAndString(t *testing.T) {
	if (LoginRateLimit{}).Enabled() {
		t.Fatal("zero config should be disabled")
	}
	if (LoginRateLimit{MaxFailures: 10}).Enabled() {
		t.Fatal("missing window should be disabled")
	}
	cfg := LoginRateLimit{MaxFailures: 10, Window: 15 * time.Minute}
	if !cfg.Enabled() {
		t.Fatal("10/15m should be enabled")
	}
	if got := cfg.String(); got != "10/15m0s" && got != "10/15m" {
		// time.Duration.String() 对整分钟是 "15m0s"；两种都接受，只要求稳定可读。
		t.Fatalf("String() = %q", got)
	}
	if got := (LoginRateLimit{}).String(); got != "off" {
		t.Fatalf("disabled String() = %q, want off", got)
	}
}

func TestLoginLimiterTokenBucket(t *testing.T) {
	limiter := newLoginLimiter(LoginRateLimit{MaxFailures: 3, Window: time.Minute})
	now := time.Unix(1_700_000_000, 0)
	limiter.now = func() time.Time { return now }

	key := loginRateKey("192.0.2.10", "Alice")
	// 用户名大小写归一：换大小写不能换桶。
	if key != loginRateKey("192.0.2.10", "alice") {
		t.Fatalf("rate key is not case-insensitive: %q vs %q", key, loginRateKey("192.0.2.10", "alice"))
	}
	// 不同 IP/用户名得到不同桶。
	if key == loginRateKey("192.0.2.11", "alice") || key == loginRateKey("192.0.2.10", "bob") {
		t.Fatal("rate key must include both IP and username")
	}

	// 容量 3：前 3 次放行且不需要等待。
	for i := 0; i < 3; i++ {
		ok, retry := limiter.allow(key)
		if !ok {
			t.Fatalf("attempt %d denied, want allowed", i+1)
		}
		if retry != 0 {
			t.Fatalf("attempt %d retryAfter = %v, want 0", i+1, retry)
		}
	}

	// 第 4 次拒绝；回满一个令牌需要 Window/容量 = 20s。
	ok, retry := limiter.allow(key)
	if ok {
		t.Fatal("4th attempt allowed, want denied")
	}
	if retry < 19*time.Second || retry > 21*time.Second {
		t.Fatalf("retryAfter = %v, want ~20s", retry)
	}

	// 等 20s → 回一个令牌 → 放行。
	now = now.Add(20 * time.Second)
	if ok, _ := limiter.allow(key); !ok {
		t.Fatal("attempt after partial refill denied, want allowed")
	}

	// 成功登录清零：reset 后桶立即恢复满容量。
	limiter.reset(key)
	for i := 0; i < 3; i++ {
		if ok, _ := limiter.allow(key); !ok {
			t.Fatalf("attempt %d after reset denied, want allowed", i+1)
		}
	}

	// 其它键不受影响。
	if ok, _ := limiter.allow(loginRateKey("192.0.2.10", "bob")); !ok {
		t.Fatal("independent bucket denied")
	}
}

func TestLoginLimiterCleansUpIdleBuckets(t *testing.T) {
	limiter := newLoginLimiter(LoginRateLimit{MaxFailures: 2, Window: time.Minute})
	now := time.Unix(1_700_000_000, 0)
	limiter.now = func() time.Time { return now }

	if ok, _ := limiter.allow("offender"); !ok {
		t.Fatal("first attempt denied")
	}
	// 前进两个窗口后任意一次 allow 会触发清理：空闲超过一个窗口的桶应被删除。
	now = now.Add(2 * time.Minute)
	if ok, _ := limiter.allow("someone-else"); !ok {
		t.Fatal("second attempt denied")
	}
	if _, exists := limiter.buckets["offender"]; exists {
		t.Fatal("idle bucket was not garbage-collected")
	}
	if _, exists := limiter.buckets["someone-else"]; !exists {
		t.Fatal("active bucket was garbage-collected")
	}
}
