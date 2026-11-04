package server

import "partyup/internal/protocol"

// handleServerInfo 应答 server.info（PROTOCOL.md §1.4）：下发服务端的**配置**，
// 目前只有 STUN 地址。
//
// 为什么单独一条消息、而不是塞进 voice.state：
//   - 时序：stun_urls 必须在建 PeerConnection **之前**拿到（要写进 ICE servers 配置），
//     而 voice.state 是加入语音房间之后才广播的；
//   - 归属：STUN 是服务器级属性，屏幕直连也要用，不该耦合到语音房间状态里。
//
// 它**只回配置**：客户端不能据此推断自己能选 relay/direct（那是 voice.state.transport
// 与服务端策略的事）。空数组表示运维没开 STUN。
func (c *conn) handleServerInfo(env protocol.Envelope) {
	c.hub.mu.RLock()
	urls := append([]string(nil), c.hub.stunURLs...)
	c.hub.mu.RUnlock()
	if urls == nil {
		// 空数组而不是 null：客户端（以及后面可能的第三方实现）不必区分两者。
		urls = []string{}
	}
	c.replyOK(env, protocol.ServerInfoOK{StunURLs: urls})
}
