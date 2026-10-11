FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
RUN CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o /out/biaseddoom-online ./cmd/biaseddoom-online

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/biaseddoom-online /biaseddoom-online
EXPOSE 8080/tcp 3478/udp
ENTRYPOINT ["/biaseddoom-online"]
