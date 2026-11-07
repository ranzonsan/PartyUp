# PartyUp 服务端容器镜像
#
# 设计要点
#   * 服务端是纯 Go、CGO_ENABLED=0 的静态二进制（控制面 + Pion SFU 同进程），
#     所以构建用 golang 镜像、运行用 alpine，镜像很小（~20 MB）。
#   * **必须用 host 网络运行**：WebRTC/SFU 的媒体走 UDP，Pion 默认使用临时端口，
#     bridge 网络下容器内的候选地址（172.17.x.x）客户端不可达。详见 docs/DOCKER.md。
#   * 数据目录挂 /data 卷；bootstrap token / INVITE_CODE 每次首次启动打印到 stdout。
#   * 版本号由 build-arg VERSION 注入（与仓库根 VERSION、git tag 三者一致）。

FROM golang:1.27-alpine AS build
WORKDIR /src

# 先拷 go.mod/go.sum 单独下依赖，改代码时能命中级联缓存
COPY server/go.mod server/go.sum ./
RUN go mod download

COPY server/ ./

ARG VERSION=dev
RUN CGO_ENABLED=0 GOOS=linux go build \
      -trimpath \
      -ldflags "-s -w -X main.version=${VERSION}" \
      -o /out/partyup \
      ./cmd/partyup

FROM alpine:3.20

# ca-certificates：将来反代/STUN over TLS 之类的出站 TLS 校验要用
# tzdata：日志时间按容器时区，用户挂 TZ 环境变量即可
RUN apk add --no-cache ca-certificates tzdata \
 && adduser -D -u 10001 -h /data partyup

COPY --from=build /out/partyup /usr/local/bin/partyup

USER partyup
WORKDIR /data
VOLUME ["/data"]

# 8080/tcp 控制面（HTTP+WebSocket）；3478/udp 内建 STUN。媒体 UDP 端口是动态的，
# 所以这些 EXPOSE 只是说明，真正的网络隔离要靠 host 网络。
EXPOSE 8080 3478/udp

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8080/api/server-info >/dev/null || exit 1

ENTRYPOINT ["partyup"]
CMD ["serve", "--data", "/data", "--http", "0.0.0.0:8080", "--stun-listen", ":3478"]
