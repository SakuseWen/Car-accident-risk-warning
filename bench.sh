#chmod +x bench.sh
#./bench.sh



#!/usr/bin/env bash
set -euo pipefail

# ——— 编译器及选项 ———
CC="gcc -O2 -pipe -Wall -pthread -std=c99 \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    -lcurl -lcjson"

# ——— 源文件 & 可执行文件名 ———
OLD_SRC="OP_Old.c"
NEW_SRC="OP.c"
FIXED_SRC="OP_fixed.c"
OLD_BIN="old"
NEW_BIN="new"

# ——— 基准测试时长（秒） & 报告文件 ———
DURATION=10
REPORT="bench-report.txt"

# 清空旧报告
echo "Benchmark @ $(date)" > "$REPORT"

# ① 复制并修正新版源码（OP.c → OP_fixed.c）
cp "$NEW_SRC" "$FIXED_SRC"
sed -i '' \
  -e 's/write_cb/write_callback/g' \
  -e 's/log_enqueue/logEvent/g' \
  -e 's/export_geojson_if_changed/exportRiskGeoJSON/g' \
  -e 's/init_random_accident/initialize/g' \
  -e 's/traffic_worker/fetchTraffic/g' \
  -e 's/weather_worker/fetchWeather/g' \
  -e 's/chicago_worker/fetchChicagoVolume/g' \
  -e 's/analyze_worker/analyzeRisk/g' \
  "$FIXED_SRC"

# ② 编译旧版与新版（修正后）
echo "Compiling $OLD_SRC → $OLD_BIN" | tee -a "$REPORT"
$CC "$OLD_SRC" -o "$OLD_BIN" 2>&1 | tee -a "$REPORT"

echo "Compiling $FIXED_SRC → $NEW_BIN" | tee -a "$REPORT"
$CC "$FIXED_SRC" -o "$NEW_BIN" 2>&1 | tee -a "$REPORT"

# ③ 基准测试函数（用 setsid 启动子进程组，超时后统一杀掉）
run_bench(){
  local exe=$1 label=$2
  echo -e "\n### $label" >> "$REPORT"

  # setsid 让程序成为新会话领导者，其所有线程/子进程同属一进程组
  { /usr/bin/time -l setsid ./$exe; } 2>> "$REPORT" &
  pid=$!

  # 如果安装了 pidstat，可同时记录 CPU/内存
  if command -v pidstat &>/dev/null; then
    pidstat -rud -p $pid 1 $DURATION >> "$REPORT" &
    mon=$!
  fi

  # 等待指定时长
  sleep $DURATION

  # 先发 SIGTERM 给整个进程组（负号表示组号）
  kill -TERM -"$pid" 2>/dev/null || true
  sleep 1
  # 再发 SIGKILL 强制结束
  kill -KILL -"$pid" 2>/dev/null || true

  # 回收子进程
  wait $pid 2>/dev/null || true
}

# 分别测试 legacy 与 optimized
run_bench "$OLD_BIN" "legacy"
run_bench "$NEW_BIN" "optimized"

echo -e "\nDone. 结果已写入 $REPORT"
