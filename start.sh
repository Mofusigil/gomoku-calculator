#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BIND="${BIND:-127.0.0.1}"
PORT="${PORT:-8080}"
URL="http://$BIND:$PORT"
SERVER_PID=""
SESSION_STATUS_URL="$URL/api/session/status"

open_browser() {
    if [[ "${OPEN_BROWSER:-1}" == "0" ]]; then
        return
    fi

    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$URL" >/dev/null 2>&1 &
    elif command -v gio >/dev/null 2>&1; then
        gio open "$URL" >/dev/null 2>&1 &
    elif command -v open >/dev/null 2>&1; then
        open "$URL" >/dev/null 2>&1 &
    else
        printf '未找到浏览器启动命令，请手动打开：%s\n' "$URL"
    fi
}

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

session_status() {
    curl --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 2 --request POST \
        --header 'Content-Type: application/json' \
        --data '{}' "$SESSION_STATUS_URL" 2>/dev/null || true
}

wait_for_browser_close() {
    local seen_session=0
    local inactive_checks=0
    local startup_checks=0

    printf '等待网页连接...\n'
    while kill -0 "$SERVER_PID" 2>/dev/null; do
        case "$(session_status)" in
            200)
                if ((seen_session == 0)); then
                    printf '网页已连接；关闭所有项目网页后，服务将自动停止。\n'
                fi
                seen_session=1
                inactive_checks=0
                ;;
            204)
                if ((seen_session == 1)); then
                    ((inactive_checks += 1))
                    if ((inactive_checks >= 5)); then
                        printf '网页已关闭，正在停止服务...\n'
                        return 0
                    fi
                else
                    ((startup_checks += 1))
                    if ((startup_checks >= 60)); then
                        printf '网页未能在 60 秒内连接，正在停止服务。\n' >&2
                        return 1
                    fi
                fi
                ;;
        esac
        sleep 1
    done

    wait "$SERVER_PID" || true
    SERVER_PID=""
    printf '服务进程已意外退出。\n' >&2
    return 1
}

if curl --silent --fail --max-time 1 "$URL/api/health" >/dev/null 2>&1; then
    printf '服务已在运行：%s\n' "$URL"
    open_browser
    exit 0
fi

printf '正在配置并编译项目...\n'
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target gomoku_server --parallel

printf '正在启动服务：%s\n' "$URL"
"$BUILD_DIR/gomoku_server" --bind "$BIND" --port "$PORT" --web-root "$ROOT_DIR/web" &
SERVER_PID=$!

for ((attempt = 1; attempt <= 100; attempt++)); do
    if curl --silent --fail --max-time 1 "$URL/api/health" >/dev/null 2>&1; then
        printf '服务已就绪，正在打开浏览器...\n'
        open_browser
        if [[ "${AUTO_STOP:-1}" == "0" ]]; then
            printf '按 Ctrl+C 停止服务。\n'
            if wait "$SERVER_PID"; then
                status=0
            else
                status=$?
            fi
        elif wait_for_browser_close; then
            status=0
        else
            status=$?
        fi
        exit "$status"
    fi

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        wait "$SERVER_PID" || true
        SERVER_PID=""
        printf '服务启动失败，请检查上方错误信息。\n' >&2
        exit 1
    fi

    sleep 0.1
done

printf '服务启动超时。\n' >&2
exit 1
