package protocol

import (
	"encoding/json"
	"strings"
	"testing"
)

// TestMediaSignalRequestGenerationJSONContract 锁定 B2b 的 generation 线上合同：
//   - 新客户端显式带 generation 时原样落在 JSON 里；
//   - 旧客户端不带该字段仍能解码为 0（legacy 路径完全不受影响）；
//   - 非严格信令序列化时不产生 generation 键（omitempty，不改变旧报文形状）。
func TestMediaSignalRequestGenerationJSONContract(t *testing.T) {
	req := MediaSignalRequest{
		ChannelID:    "chan-1",
		To:           "viewer-1",
		Media:        "screen",
		Kind:         "answer",
		SessionID:    "receive-1",
		SessionNonce: "nonce-1",
		Generation:   7,
		Payload:      `{"type":"answer"}`,
	}
	raw, err := json.Marshal(req)
	if err != nil {
		t.Fatalf("marshal media signal request: %v", err)
	}
	if !strings.Contains(string(raw), `"generation":7`) {
		t.Fatalf("marshaled media signal request lacks generation: %s", raw)
	}
	var round MediaSignalRequest
	if err := json.Unmarshal(raw, &round); err != nil {
		t.Fatalf("unmarshal media signal request: %v", err)
	}
	if round.Generation != 7 {
		t.Fatalf("generation round trip = %d, want 7", round.Generation)
	}

	// 旧客户端/旧路径报文：没有 generation 字段，解码后是 0，不做未知字段假设。
	legacy := []byte(`{"channel_id":"chan-1","media":"screen","kind":"offer","payload":"{}"}`)
	var old MediaSignalRequest
	if err := json.Unmarshal(legacy, &old); err != nil {
		t.Fatalf("unmarshal legacy media signal request: %v", err)
	}
	if old.Generation != 0 {
		t.Fatalf("legacy request generation = %d, want 0", old.Generation)
	}
	if old.SessionID != "" || old.SessionNonce != "" {
		t.Fatalf("legacy request unexpectedly carries session identity: %+v", old)
	}
	encodedOld, err := json.Marshal(old)
	if err != nil {
		t.Fatalf("marshal legacy media signal request: %v", err)
	}
	if strings.Contains(string(encodedOld), `"generation"`) {
		t.Fatalf("legacy-shaped request must omit generation (omitempty): %s", encodedOld)
	}
}

func TestMediaSignalPushGenerationJSONContract(t *testing.T) {
	push := MediaSignalPush{
		ChannelID:    "chan-1",
		From:         "sharer-1",
		Media:        "screen",
		Kind:         MediaSignalKindViewerArm,
		SessionID:    "receive-1",
		SessionNonce: "nonce-1",
		Generation:   3,
	}
	raw, err := json.Marshal(push)
	if err != nil {
		t.Fatalf("marshal media signal push: %v", err)
	}
	if !strings.Contains(string(raw), `"generation":3`) {
		t.Fatalf("viewer_arm push lacks generation: %s", raw)
	}
}
