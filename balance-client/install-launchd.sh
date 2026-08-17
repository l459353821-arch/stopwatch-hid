#!/bin/bash
# 安装 launchd 定时任务：每 5 分钟把 DeepSeek 余额推送到 StopWatch。
# 用法: bash install-launchd.sh <DEEPSEEK_API_KEY>
set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
  echo "用法: bash install-launchd.sh <DEEPSEEK_API_KEY>"
  exit 1
fi
KEY="$1"

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/.build/debug/ds-balance"
LABEL="com.stopwatch.ds-balance"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_OUT="$HOME/Library/Logs/ds-balance.out.log"
LOG_ERR="$HOME/Library/Logs/ds-balance.err.log"

echo "== 编译 ds-balance =="
( cd "$DIR" && swift build )

mkdir -p "$HOME/Library/LaunchAgents"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>--push-balance</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>DEEPSEEK_API_KEY</key>
        <string>$KEY</string>
    </dict>
    <key>StartInterval</key>
    <integer>300</integer>
    <key>RunAtLoad</key>
    <true/>
    <key>ProcessType</key>
    <string>Background</string>
    <key>StandardOutPath</key>
    <string>$LOG_OUT</string>
    <key>StandardErrorPath</key>
    <string>$LOG_ERR</string>
</dict>
</plist>
EOF

UID_NUM=$(id -u)
launchctl bootout gui/$UID_NUM/$LABEL 2>/dev/null || true
launchctl bootstrap gui/$UID_NUM "$PLIST"
launchctl enable gui/$UID_NUM/$LABEL

echo "已安装：$PLIST（每 5 分钟自动推送一次）"
echo "查看日志：tail -f $LOG_OUT"
echo "卸载：launchctl bootout gui/$UID_NUM/$LABEL && rm $PLIST"
