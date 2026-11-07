package main

// REG-03 CLI 回归：invite 子命令的旧输出必须逐字节不变；
// 只有显式 --server-url 才额外输出 INVITE_LINK；非法 URL 在落库前被拒绝。

import (
	"context"
	"errors"
	"io"
	"net/url"
	"os"
	"strings"
	"testing"

	"partyup/internal/invite"
	"partyup/internal/store"
)

// runInvite 捕获 cmdInvite 的 stdout（它直接写 os.Stdout，与真实 CLI 一致）。
// 不用 bytes.Buffer 包一层生产逻辑，避免为了测试改输出路径。
func runInvite(t *testing.T, args ...string) (string, error) {
	t.Helper()
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("pipe: %v", err)
	}
	os.Stdout = w
	defer func() { os.Stdout = old }()

	runErr := cmdInvite(args)
	if err := w.Close(); err != nil {
		t.Fatalf("close stdout pipe: %v", err)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("read stdout: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("close stdout reader: %v", err)
	}
	return string(out), runErr
}

func stdoutLines(out string) []string {
	trimmed := strings.TrimSuffix(out, "\n")
	if trimmed == "" {
		return nil
	}
	return strings.Split(trimmed, "\n")
}

func valueOf(t *testing.T, line, prefix string) string {
	t.Helper()
	value, ok := strings.CutPrefix(line, prefix)
	if !ok || value == "" {
		t.Fatalf("line %q does not start with %q", line, prefix)
	}
	return value
}

// TestInviteWithoutServerURLKeepsLegacyStdout 锁定旧行为：
// 无 --server-url 时 stdout 只有一行 INVITE_CODE，不出现 INVITE_LINK。
func TestInviteWithoutServerURLKeepsLegacyStdout(t *testing.T) {
	dir := t.TempDir()
	out, err := runInvite(t, "--data", dir)
	if err != nil {
		t.Fatalf("cmdInvite: %v", err)
	}
	lines := stdoutLines(out)
	if len(lines) != 1 {
		t.Fatalf("stdout = %q, want exactly one INVITE_CODE line", out)
	}
	if strings.Contains(out, "INVITE_LINK") {
		t.Fatalf("stdout unexpectedly contains INVITE_LINK: %q", out)
	}
	code := valueOf(t, lines[0], "INVITE_CODE=")

	// 邀请码必须真的落库（stdout 之外仍可用）。
	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer st.Close()
	inv, err := st.InviteByCode(context.Background(), code)
	if err != nil {
		t.Fatalf("invite %q not stored: %v", code, err)
	}
	if inv.UsedBy != "" || inv.UsedAt != 0 {
		t.Fatalf("fresh invite is marked used: %+v", inv)
	}
}

// TestInviteWithServerURLEmitsLink 锁定新增行为：给出 --server-url 时，
// 在 INVITE_CODE 之后额外输出与生产 builder 完全一致的 INVITE_LINK。
func TestInviteWithServerURLEmitsLink(t *testing.T) {
	dir := t.TempDir()
	const serverURL = "https://example.org/partyup"
	out, err := runInvite(t, "--data", dir, "--server-url", serverURL)
	if err != nil {
		t.Fatalf("cmdInvite: %v", err)
	}
	lines := stdoutLines(out)
	if len(lines) != 2 {
		t.Fatalf("stdout = %q, want INVITE_CODE + INVITE_LINK", out)
	}
	code := valueOf(t, lines[0], "INVITE_CODE=")
	link := valueOf(t, lines[1], "INVITE_LINK=")

	want, err := invite.BuildLink(serverURL, code)
	if err != nil {
		t.Fatalf("BuildLink: %v", err)
	}
	if link != want {
		t.Fatalf("INVITE_LINK = %q, want %q", link, want)
	}

	// 用标准库复核链接形状：base path 保留、query 恰好一个 code。
	u, err := url.Parse(link)
	if err != nil {
		t.Fatalf("parse INVITE_LINK: %v", err)
	}
	if u.Path != "/partyup/" {
		t.Fatalf("link path = %q, want /partyup/", u.Path)
	}
	query := u.Query()
	if len(query) != 1 || len(query["code"]) != 1 || query.Get("code") != code {
		t.Fatalf("link query = %v, want only code=%s", query, code)
	}

	st, err := store.Open(dir)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer st.Close()
	if _, err := st.InviteByCode(context.Background(), code); err != nil {
		t.Fatalf("invite %q not stored: %v", code, err)
	}
}

// TestInviteRejectsInvalidServerURLWithoutStoring 锁定顺序：URL 校验在创建码之前。
// 失败时 stdout 必须为空，库里不得留下任何未使用邀请码。
func TestInviteRejectsInvalidServerURLWithoutStoring(t *testing.T) {
	for _, serverURL := range []string{
		" ",
		"ftp://example.org/partyup",
		"https://example.org/partyup?x=1",
		"https://example.org:0/partyup",
		"https://user:pass@example.org/partyup",
		"https://example.org/partyup#frag",
		"example.org/partyup",
	} {
		t.Run(serverURL, func(t *testing.T) {
			dir := t.TempDir()
			out, err := runInvite(t, "--data", dir, "--server-url", serverURL)
			if err == nil {
				t.Fatalf("cmdInvite accepted invalid server URL %q (stdout=%q)", serverURL, out)
			}
			if out != "" {
				t.Fatalf("stdout = %q, want empty on rejected URL", out)
			}

			st, err := store.Open(dir)
			if err != nil {
				t.Fatalf("open store: %v", err)
			}
			defer st.Close()
			if _, err := st.FirstUnusedInvite(context.Background()); !errors.Is(err, store.ErrNotFound) {
				t.Fatalf("invalid URL still created an invite: err=%v", err)
			}
		})
	}
}

// TestInviteEmptyServerURLKeepsLegacyStdout：显式传空字符串等同于未给参数，
// 不生成也不猜测链接（防止部署脚本传空变量时静默造出错误链接）。
func TestInviteEmptyServerURLKeepsLegacyStdout(t *testing.T) {
	dir := t.TempDir()
	out, err := runInvite(t, "--data", dir, "--server-url", "")
	if err != nil {
		t.Fatalf("cmdInvite: %v", err)
	}
	lines := stdoutLines(out)
	if len(lines) != 1 {
		t.Fatalf("stdout = %q, want only INVITE_CODE", out)
	}
	if strings.Contains(out, "INVITE_LINK") {
		t.Fatalf("stdout unexpectedly contains INVITE_LINK: %q", out)
	}
}
