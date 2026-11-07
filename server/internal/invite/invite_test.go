package invite_test

// REG-03 生成端回归：邀请链接格式、base path 保留、转义与拒绝规则。
// 生成/导入两侧共用 tests/fixtures/invite-link-cases.json（标准 JSON），
// 本文件只校验 Go 生成端；C# 导入端在同一 fixture 上校验 parse/往返。

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"partyup/internal/invite"
)

type linkCase struct {
	Server string `json:"server"`
	Code   string `json:"code"`
	Base   string `json:"base"`
	Link   string `json:"link"`
}

type fixture struct {
	Build       []linkCase `json:"build"`
	BuildReject []string   `json:"build_reject"`
	Parse       []linkCase `json:"parse"`
	ParseReject []string   `json:"parse_reject"`
}

func loadFixture(t *testing.T) fixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "tests", "fixtures", "invite-link-cases.json"))
	if err != nil {
		t.Fatalf("read invite link fixture: %v", err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatalf("decode invite link fixture: %v", err)
	}
	if len(f.Build) == 0 || len(f.BuildReject) == 0 {
		t.Fatalf("fixture is missing build cases: build=%d reject=%d", len(f.Build), len(f.BuildReject))
	}
	return f
}

// TestBuildLinkMatchesCrossLanguageFixture 锁定生成链接的规范形：
// 绝对 http(s)、保留 base path 目录尾斜杠、query 里恰好一个 code。
func TestBuildLinkMatchesCrossLanguageFixture(t *testing.T) {
	f := loadFixture(t)
	for _, tc := range f.Build {
		t.Run(tc.Link, func(t *testing.T) {
			got, err := invite.BuildLink(tc.Server, tc.Code)
			if err != nil {
				t.Fatalf("BuildLink(%q, %q): %v", tc.Server, tc.Code, err)
			}
			if got != tc.Link {
				t.Fatalf("BuildLink(%q, %q) = %q, want %q", tc.Server, tc.Code, got, tc.Link)
			}

			base, err := invite.ParseServerBase(tc.Server)
			if err != nil {
				t.Fatalf("ParseServerBase(%q): %v", tc.Server, err)
			}
			if base.String() != tc.Base {
				t.Fatalf("ParseServerBase(%q).String() = %q, want %q", tc.Server, base.String(), tc.Base)
			}
		})
	}
}

// TestBuildLinkRejectsInvalidServerURLs 锁定拒绝面：缺 scheme、非 http(s)、
// user-info、已有 query/fragment、非法端口、控制字符/空白都不产生链接。
// 错误信息不得回显整条链接或邀请码。
func TestBuildLinkRejectsInvalidServerURLs(t *testing.T) {
	f := loadFixture(t)
	const code = "K7QM3XPD"
	for _, raw := range f.BuildReject {
		t.Run(raw, func(t *testing.T) {
			got, err := invite.BuildLink(raw, code)
			if err == nil {
				t.Fatalf("BuildLink(%q) = %q, want error", raw, got)
			}
			if got != "" {
				t.Fatalf("BuildLink(%q) returned link %q alongside error %v", raw, got, err)
			}
			if strings.Contains(err.Error(), code) {
				t.Fatalf("error leaks invite code: %v", err)
			}
			if strings.Contains(strings.ToLower(err.Error()), "code=") {
				t.Fatalf("error leaks link query: %v", err)
			}
		})
	}
}

// TestBuildLinkEscapesCodeQuery 证明 code 由 net/url 的 query 编码器做转义，
// 不是字符串拼接：& = 空格等不能变成第二个参数。
func TestBuildLinkEscapesCodeQuery(t *testing.T) {
	got, err := invite.BuildLink("https://example.org/partyup/", "A&B=C D")
	if err != nil {
		t.Fatalf("BuildLink: %v", err)
	}
	const want = "https://example.org/partyup/?code=A%26B%3DC+D"
	if got != want {
		t.Fatalf("BuildLink = %q, want %q", got, want)
	}
}

// TestBuildLinkRejectsEmptyCode：空/空白邀请码不应拼出 ?code=。
func TestBuildLinkRejectsEmptyCode(t *testing.T) {
	for _, code := range []string{"", "   "} {
		if got, err := invite.BuildLink("https://example.org/partyup/", code); err == nil {
			t.Fatalf("BuildLink(code=%q) = %q, want error", code, got)
		}
	}
}
