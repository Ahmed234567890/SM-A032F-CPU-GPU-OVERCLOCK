#!/system/bin/sh
# Cool the phone to a target temperature, then report the CPU policy state.
# Run as root. Synchronous; no background processes.
#
# usage: bench_cool.sh [target_millic]
#   target_millic defaults to 45000 (45.0 C)

TARGET="${1:-45000}"

# Screen off reduces background activity and display heat.
input keyevent 223 >/dev/null 2>&1

i=0
while [ "$i" -lt 60 ]; do
    T=$(cat /sys/class/thermal/thermal_zone1/temp 2>/dev/null)
    echo "cool[$i] temp=$T"
    [ -n "$T" ] && [ "$T" -le "$TARGET" ] && break
    sleep 10
    i=$((i + 1))
done

for p in policy0 policy4; do
    echo "cool done: $p governor=$(cat /sys/devices/system/cpu/cpufreq/$p/scaling_governor 2>/dev/null)"
done

echo "ready temp=$(cat /sys/class/thermal/thermal_zone1/temp 2>/dev/null) p0max=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null) p4max=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq 2>/dev/null) p0cur=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null) p4cur=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq 2>/dev/null)"
