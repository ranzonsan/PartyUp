package auth_test

import (
	"strings"
	"testing"

	"partyup/internal/auth"
)

func TestPasswordHashRoundTrip(t *testing.T) {
	const password = "correct horse battery staple"

	encoded, err := auth.HashPassword(password)
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}
	if !strings.HasPrefix(encoded, "$argon2id$") {
		t.Fatalf("encoded hash = %q, want $argon2id$ PHC prefix", encoded)
	}
	if strings.Contains(encoded, password) {
		t.Fatal("encoded hash leaks the plaintext password")
	}

	ok, err := auth.VerifyPassword(encoded, password)
	if err != nil {
		t.Fatalf("verify password: %v", err)
	}
	if !ok {
		t.Fatal("verify returned false for the correct password")
	}

	ok, err = auth.VerifyPassword(encoded, "wrong password")
	if err != nil {
		t.Fatalf("verify wrong password: %v", err)
	}
	if ok {
		t.Fatal("verify returned true for a wrong password")
	}
}

func TestPasswordHashIsSalted(t *testing.T) {
	a, err := auth.HashPassword("same-password")
	if err != nil {
		t.Fatalf("hash a: %v", err)
	}
	b, err := auth.HashPassword("same-password")
	if err != nil {
		t.Fatalf("hash b: %v", err)
	}
	if a == b {
		t.Fatal("two hashes of the same password are identical — salt missing")
	}
}

func TestInviteCodeFormat(t *testing.T) {
	seen := make(map[string]bool, 32)
	for i := 0; i < 32; i++ {
		code, err := auth.NewInviteCode()
		if err != nil {
			t.Fatalf("generate invite code: %v", err)
		}
		if len(code) != 10 {
			t.Fatalf("invite code %q has length %d, want 10", code, len(code))
		}
		if code != strings.ToUpper(code) {
			t.Fatalf("invite code %q is not upper case", code)
		}
		for _, r := range code {
			if !(r >= 'A' && r <= 'Z') && !(r >= '2' && r <= '7') && !(r >= '8' && r <= '9') && !(r >= '0' && r <= '1') {
				// 允许字母表内的字符；这里只排除明显不属于 base32 的符号。
				if r < '0' || r > 'Z' {
					t.Fatalf("invite code %q contains unexpected rune %q", code, r)
				}
			}
		}
		if seen[code] {
			t.Fatalf("duplicate invite code generated: %s", code)
		}
		seen[code] = true
	}
}

func TestNormalizeInviteCode(t *testing.T) {
	cases := map[string]string{
		"k7qm-3xpd":    "K7QM3XPD",
		" K7QM3XPD ":   "K7QM3XPD",
		"k7qm_3xpd":    "K7QM3XPD",
		"K7QM 3XPD":    "K7QM3XPD",
		"alreadyupper": "ALREADYUPPER",
	}
	for in, want := range cases {
		if got := auth.NormalizeInviteCode(in); got != want {
			t.Errorf("NormalizeInviteCode(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestValidName(t *testing.T) {
	valid := []string{"alice", "Alice_99", "bob-2", "张三", "玩家一号"}
	invalid := []string{"", "a", strings.Repeat("x", 33), "has space", "emoji😀", "tab\tname"}

	for _, name := range valid {
		if !auth.ValidName(name) {
			t.Errorf("ValidName(%q) = false, want true", name)
		}
	}
	for _, name := range invalid {
		if auth.ValidName(name) {
			t.Errorf("ValidName(%q) = true, want false", name)
		}
	}
}

func TestValidDisplayName(t *testing.T) {
	valid := []string{
		"a",
		"Alice",
		"Alice 🐱",
		"张三",
		"with space",
		".-_'\"()[]{}",
		strings.Repeat("名", auth.MaxDisplayNameLen),
	}
	invalid := []string{
		"",
		strings.Repeat("名", auth.MaxDisplayNameLen+1),
		"line\nbreak",
		"tab\tname",
		"nul\x00byte",
		"zero\u200bwidth",
	}

	for _, name := range valid {
		if !auth.ValidDisplayName(name) {
			t.Errorf("ValidDisplayName(%q) = false, want true", name)
		}
	}
	for _, name := range invalid {
		if auth.ValidDisplayName(name) {
			t.Errorf("ValidDisplayName(%q) = true, want false", name)
		}
	}
}

func TestValidPassword(t *testing.T) {
	if auth.ValidPassword("12345") {
		t.Error("5-byte password should be rejected")
	}
	if !auth.ValidPassword("123456") {
		t.Error("6-byte password should be accepted")
	}
	if auth.ValidPassword(strings.Repeat("x", auth.MaxPasswordLen+1)) {
		t.Error("over-long password should be rejected")
	}
}
