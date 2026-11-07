package server_test

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

// TestInviteValidateIsNonConsuming 锁定邀请码预检的核心语义：
// 有效 200 {valid:true}；无效 404；已用 409。预检不建成员、不消费邀请码，
// 之后真正的 POST /api/invite/{code} 仍能成功（它自己在事务里重验 + 消费）。
func TestInviteValidateIsNonConsuming(t *testing.T) {
	env := newTestEnv(t)
	env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()

	ctx := context.Background()
	before, err := env.store.MemberCount(ctx)
	if err != nil {
		t.Fatalf("member count before: %v", err)
	}

	// 大小写/分隔符归一化后仍应命中（与注册路径共用 NormalizeInviteCode）。
	messy := strings.ToLower(code[:4]) + "-" + strings.ToLower(code[4:])
	status, body := env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]string{"code": messy})
	if status != http.StatusOK {
		t.Fatalf("validate valid code: status=%d body=%s, want 200", status, body)
	}
	var valid struct {
		Valid bool `json:"valid"`
	}
	if err := json.Unmarshal(body, &valid); err != nil {
		t.Fatalf("decode validate response: %v", err)
	}
	if !valid.Valid {
		t.Fatalf("validate.valid = false, want true (body=%s)", body)
	}
	// 响应只应包含 valid，不应泄露创建者/使用者等成员信息。
	var raw map[string]any
	if err := json.Unmarshal(body, &raw); err != nil {
		t.Fatalf("decode raw validate response: %v", err)
	}
	if len(raw) != 1 {
		t.Fatalf("validate response keys = %v, want only valid", raw)
	}

	// 预检不消费：邀请码仍未被使用，成员数不变。
	inv, err := env.store.InviteByCode(ctx, code)
	if err != nil {
		t.Fatalf("invite still exists: %v", err)
	}
	if inv.UsedBy != "" || inv.UsedAt != 0 {
		t.Fatalf("validate consumed invite: used_by=%q used_at=%d", inv.UsedBy, inv.UsedAt)
	}
	after, err := env.store.MemberCount(ctx)
	if err != nil {
		t.Fatalf("member count after: %v", err)
	}
	if after != before {
		t.Fatalf("member count changed by validate: before=%d after=%d", before, after)
	}

	// 预检成功后正式注册仍能成功——预检不代表占用。
	member, _ := env.registerViaInvite(code, "bob", "secret123")
	if member.Name != "bob" {
		t.Fatalf("registered member = %+v, want bob", member)
	}

	// 已消费后再预检：409 CONFLICT。
	status, body = env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]string{"code": code})
	if status != http.StatusConflict {
		t.Fatalf("validate used code: status=%d body=%s, want 409", status, body)
	}
	var errResp errorResponse
	if err := json.Unmarshal(body, &errResp); err != nil {
		t.Fatalf("decode conflict: %v", err)
	}
	if errResp.Code != "CONFLICT" {
		t.Fatalf("used code error = %q, want CONFLICT", errResp.Code)
	}
}

// TestInviteValidateRejectsBadInput 锁定无效/格式错误的输入：
// 未知码 404、空码 404、非法 JSON 400。错误响应同样只有 code/message。
func TestInviteValidateRejectsBadInput(t *testing.T) {
	env := newTestEnv(t)

	status, body := env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]string{"code": "NOPE000000"})
	if status != http.StatusNotFound {
		t.Fatalf("validate unknown code: status=%d body=%s, want 404", status, body)
	}

	status, body = env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]string{"code": "  --  "})
	if status != http.StatusNotFound {
		t.Fatalf("validate empty normalized code: status=%d body=%s, want 404", status, body)
	}

	status, body = env.doJSON(http.MethodPost, "/api/invite/validate", "", map[string]any{"code": 42})
	if status != http.StatusBadRequest {
		t.Fatalf("validate wrong type: status=%d body=%s, want 400", status, body)
	}
}
