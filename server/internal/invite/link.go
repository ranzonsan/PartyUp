// Package invite 生成管理员发给新成员的邀请链接（REG-03 合同）。
//
// 链接格式是服务器绝对 HTTP(S) base URL 加恰好一个非空 code 查询参数：
//
//	https://example.org/partyup/?code=ABCDE12345
//
// 它不是浏览器注册页面，也不是 partyup:// 深链；链接里的服务器地址只是连接
// 目标，不代表已信任该服务器。生成链接不新增发码接口——邀请码仍由 store
// 创建，是否可用仍由 POST /api/invite/validate 与最终注册事务裁决。
//
// URL 的解析、规范化与转义全部交给 net/url；本包只做策略校验，不手写 parser。
package invite

import (
	"errors"
	"net/url"
	"strconv"
	"strings"
)

// ServerBase 是通过策略校验并规范化的服务器 base URL。
// 零值不可用；所有方法都在副本上操作，不会修改接收者。
type ServerBase struct {
	u url.URL
}

// ParseServerBase 校验并规范化管理员给的服务器地址。
//
// 只接受绝对 http/https、非空 host、合法端口（1–65535），拒绝 user-info、
// 已有 query/fragment、控制字符与首尾空白。base path 按目录处理：缺省变
// "/"，非空补尾斜杠；百分号编码与 IPv6 字面量原样保留（都由 net/url 处理）。
//
// 错误只描述规则，不回显原始输入：调用方可能把完整链接当参数传进来。
func ParseServerBase(raw string) (ServerBase, error) {
	var zero ServerBase
	if raw == "" {
		return zero, errors.New("invite: server URL is required")
	}
	if raw != strings.TrimSpace(raw) {
		return zero, errors.New("invite: server URL must not have leading or trailing whitespace")
	}
	for _, r := range raw {
		if r < 0x20 || r == 0x7f {
			return zero, errors.New("invite: server URL must not contain control characters")
		}
	}

	u, err := url.Parse(raw)
	if err != nil {
		return zero, errors.New("invite: server URL is not a valid URL")
	}
	u.Scheme = strings.ToLower(u.Scheme)
	if u.Scheme != "http" && u.Scheme != "https" {
		return zero, errors.New("invite: server URL scheme must be http or https")
	}
	if u.Opaque != "" || u.Host == "" || u.Hostname() == "" {
		return zero, errors.New("invite: server URL must include a host")
	}
	if u.User != nil {
		return zero, errors.New("invite: server URL must not contain user information")
	}
	if u.Fragment != "" || u.RawFragment != "" {
		return zero, errors.New("invite: server URL must not contain a fragment")
	}
	if u.RawQuery != "" || u.ForceQuery {
		return zero, errors.New("invite: server URL must not contain a query")
	}
	if port := u.Port(); port != "" {
		n, err := strconv.Atoi(port)
		if err != nil || n < 1 || n > 65535 {
			return zero, errors.New("invite: server URL port must be between 1 and 65535")
		}
	}

	// base path 当目录用：以转义形式补尾斜杠，避免把 %2F/%20 解码后重编码。
	escaped := u.EscapedPath()
	if escaped == "" {
		escaped = "/"
	}
	if !strings.HasSuffix(escaped, "/") {
		escaped += "/"
	}
	path, err := url.PathUnescape(escaped)
	if err != nil {
		return zero, errors.New("invite: server URL path is not valid")
	}
	u.Path = path
	u.RawPath = escaped

	return ServerBase{u: *u}, nil
}

// String 返回规范化的 base URL（无 query/fragment，path 以 "/" 结尾）。
func (b ServerBase) String() string {
	return b.u.String()
}

// Link 把 code 作为唯一查询参数拼到 base 上，返回绝对邀请链接。
//
// code 只做非空校验；查询编码（& = 空格 等）交给 url.Values，错误信息不回显
// 邀请码。调用方应只传服务器生成的真实邀请码。
func (b ServerBase) Link(code string) (string, error) {
	if strings.TrimSpace(code) == "" {
		return "", errors.New("invite: invite code is required")
	}
	u := b.u // 复制：不污染接收者
	q := u.Query()
	q.Set("code", code)
	u.RawQuery = q.Encode()
	u.ForceQuery = false
	return u.String(), nil
}

// BuildLink 是 ParseServerBase + ServerBase.Link 的便捷组合。
func BuildLink(serverURL, code string) (string, error) {
	base, err := ParseServerBase(serverURL)
	if err != nil {
		return "", err
	}
	return base.Link(code)
}
