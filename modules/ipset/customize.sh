#!/system/bin/sh

MODID=$(grep_prop id "$MODPATH/module.prop")
MODID_DIR="/data/adb/modules/$MODID"
MODID_UPDATE_DIR="/data/adb/modules_update/$MODID"

chmod +x "$MODID_UPDATE_DIR/bin/ipset"

set_link() {
  mkdir -p "$MODID_DIR/bin"
  cp -rf "$MODPATH/bin/ipset" "$MODID_DIR/bin/"
  chmod +x "$MODID_DIR/bin/ipset"

  if [ -n "$KSU" ] || [ -n "$APATCH" ]; then
    if [ -n "$KSU" ]; then
      SUBIN_DIR="/data/adb/ksu/bin"
    else
      SUBIN_DIR="/data/adb/ap/bin"
    fi
    
    mkdir -p "$SUBIN_DIR"
    SUBIN_LINK="$SUBIN_DIR/ipset"
    rm -f "$SUBIN_LINK"
    ln -sf "$MODID_DIR/bin/ipset" "$SUBIN_LINK"
    ui_print "- 已创建软链接"
  fi
}

  ui_print " "
  ui_print "========================================"
  ui_print "      选择ipset工作模式      "
  ui_print "========================================"
  ui_print "  音量上 (+): 挂载到系统(终端可用)"
  ui_print "  音量下 (-): 创建软链(仅脚本)"
  ui_print " "
  ui_print "  10秒后默认选择【创建软链(仅脚本)】"
  ui_print "========================================"

  local timeout=10
  local start_time=$(date +%s)
  
  while true; do
    local current_time=$(date +%s)
    if [ $((current_time - start_time)) -ge $timeout ]; then
      ui_print "- 超时，默认选择：创建软链(仅脚本)"
      set_link
      break
    fi

    local key_event=$(timeout 0.5 getevent -l 2>/dev/null)
    if echo "$key_event" | grep -q "KEY_VOLUMEUP"; then
      ui_print "已选择：挂载到系统(终端可用)，重启生效"
      mkdir -p "$MODID_UPDATE_DIR/system/"
      mv -f "$MODID_UPDATE_DIR/bin" "$MODID_UPDATE_DIR/system/bin"
      break
    elif echo "$key_event" | grep -q "KEY_VOLUMEDOWN"; then
      ui_print "已选择：创建软链(仅脚本)"
      set_link
      break
    fi
  done
