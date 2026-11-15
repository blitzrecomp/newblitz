#!/system/bin/sh
# On the RG DS, as root: one CSV line every 2 s until /data/local/tmp/sampler.stop exists.
OUT=$1
P=com.blitzrecomp.newblitz
rm -f /data/local/tmp/sampler.stop
echo "t,memavail_kb,swapfree_kb,zram_used_kb,game_pid,game_rss_kb,game_pss_kb,game_swap_kb,game_gpu_pages,gpu_total_pages,gpu_load,gpu_mhz,cpu0_mhz,load1,temp" > $OUT
while [ ! -e /data/local/tmp/sampler.stop ]; do
  t=$(date +%s)
  ma=$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)
  sf=$(grep SwapFree /proc/meminfo | tr -s ' ' | cut -d' ' -f2)
  zu=$(grep zram0 /proc/swaps | tr -s ' \t' ' ' | cut -d' ' -f4)
  pid=$(pidof $P)
  rss=0; pss=0; sw=0; gp=0
  if [ -n "$pid" ]; then
    r=$(cat /proc/$pid/smaps_rollup 2>/dev/null)
    rss=$(echo "$r" | grep '^Rss:' | tr -s ' ' | cut -d' ' -f2)
    pss=$(echo "$r" | grep '^Pss:' | tr -s ' ' | cut -d' ' -f2)
    sw=$(echo "$r" | grep '^Swap:' | tr -s ' ' | cut -d' ' -f2)
    gp=$(grep " $pid " /sys/kernel/debug/mali0/gpu_memory | tr -s ' ' | awk '{s+=$4} END {print s+0}')
  fi
  gt=$(grep '^mali0' /sys/kernel/debug/mali0/gpu_memory | tr -s ' ' | cut -d' ' -f2)
  gl=$(cat /sys/class/devfreq/fde60000.gpu/load | cut -d@ -f1)
  gf=$(( $(cat /sys/class/devfreq/fde60000.gpu/cur_freq) / 1000000 ))
  cf=$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) / 1000 ))
  la=$(cut -d' ' -f1 /proc/loadavg)
  tp=$(cat /sys/class/thermal/thermal_zone0/temp)
  echo "$t,$ma,$sf,$zu,$pid,$rss,$pss,$sw,$gp,$gt,$gl,$gf,$cf,$la,$tp" >> $OUT
  if [ -e /data/local/tmp/dark ]; then echo 25 > /sys/class/backlight/backlight/brightness; echo 25 > /sys/class/backlight/backlight1/brightness; fi
  sleep 2
done
