// Package auth 负责密码哈希、会话 token 与邀请码的生成。
//
// 密码哈希用 github.com/alexedwards/argon2id（argon2id + 每用户随机盐 +
// PHC 字符串编码）。这里刻意不自己实现 PHC 字符串拼装/解析：那是标准格式，
// 手写解析器是缺陷来源。
package auth

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base32"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"strings"
	"unicode"

	"github.com/alexedwards/argon2id"
)

// 密码长度约束。骨架取宽松值，产品化时再收紧（并配 UI 提示）。
const (
	MinPasswordLen = 6
	MaxPasswordLen = 256
	MinNameLen     = 2
	MaxNameLen     = 32

	// 显示名（display_name）与登录名（name）是两套约束：登录名唯一、限 ASCII
	// 字母/数字/下划线/连字符/CJK；显示名只用于展示，允许任意可打印 Unicode
	// （空格、标点、emoji 等），不要求唯一，也**不能**当作身份 ID。
	MinDisplayNameLen = 1
	MaxDisplayNameLen = 32
)

// HashPassword 返回 argon2id 的 PHC 编码串（含参数与盐）。
func HashPassword(password string) (string, error) {
	h, err := argon2id.CreateHash(password, argon2id.DefaultParams)
	if err != nil {
		return "", fmt.Errorf("hash password: %w", err)
	}
	return h, nil
}

// VerifyPassword 校验明文密码与 PHC 串是否匹配。
func VerifyPassword(encoded, password string) (bool, error) {
	ok, err := argon2id.ComparePasswordAndHash(password, encoded)
	if err != nil {
		return false, fmt.Errorf("verify password: %w", err)
	}
	return ok, nil
}

// NewToken 生成 32 字节随机会话 token（base64url，无填充）。
// PROTOCOL.md §1：token 是不透明随机串，存在库里、可撤销，不用 JWT。
func NewToken() (string, error) {
	buf := make([]byte, 32)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("generate token: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(buf), nil
}

// HashToken 返回 token 的 sha256 十六进制串。
// 库里不存 token 原文——备份被读走时不能直接拿来登录，与存密码哈希同理。
func HashToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

var inviteEncoding = base32.StdEncoding.WithPadding(base32.NoPadding)

// NewInviteCode 生成形如 "K7QM3XPD" 的邀请码（10 个 base32 字符 = 50 bit 熵）。
// 用 Crockford 风格去掉易混字符（I/L/O/U）后的字母表，便于人工抄写。
func NewInviteCode() (string, error) {
	buf := make([]byte, 8)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("generate invite code: %w", err)
	}
	raw := inviteEncoding.EncodeToString(buf)
	code := strings.ToUpper(raw)
	// 去掉 base32 里人工抄写易混的字符，冲突概率极低但不影响安全性上限。
	code = strings.NewReplacer("I", "7", "L", "1", "O", "0", "U", "9").Replace(code)
	return code[:10], nil
}

// NormalizeInviteCode 统一邀请码大小写并去掉分隔符，登录时容忍 "k7qm-3xpd"。
func NormalizeInviteCode(code string) string {
	code = strings.ToUpper(strings.TrimSpace(code))
	return strings.NewReplacer("-", "", " ", "", "_", "").Replace(code)
}

// ValidName 校验成员名：2–32 个字符，只允许字母、数字、下划线、连字符与中文。
func ValidName(name string) bool {
	if len([]rune(name)) < MinNameLen || len([]rune(name)) > MaxNameLen {
		return false
	}
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		case r == '_' || r == '-':
		case r >= 0x4E00 && r <= 0x9FFF: // CJK 统一表意文字
		default:
			return false
		}
	}
	return true
}

// ValidDisplayName 校验显示名：1–32 个可打印 Unicode rune。
//
// 「可打印」用 unicode.IsPrint 判定（字母/数字/标点/符号/组合记号 + ASCII 空格）；
// 控制字符、换行、制表符、零宽/格式字符（如 U+202E 双向覆盖）都不接受。显示名
// 不要求唯一，调用方应先 TrimSpace；空串表示回退到登录名，不做校验。
func ValidDisplayName(name string) bool {
	runes := []rune(name)
	if len(runes) < MinDisplayNameLen || len(runes) > MaxDisplayNameLen {
		return false
	}
	for _, r := range runes {
		if !unicode.IsPrint(r) {
			return false
		}
	}
	return true
}

// ValidPassword 校验密码长度（UTF-8 长度而非 rune 数，避免多字节绕过）。
func ValidPassword(password string) bool {
	n := len(password)
	return n >= MinPasswordLen && n <= MaxPasswordLen
}
