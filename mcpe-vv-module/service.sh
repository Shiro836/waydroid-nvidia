#!/system/bin/sh
# Minecraft Bedrock "Vibrant Visuals" unlocker (Magisk late_start service).
#
# At boot: detect the GPU's GL_RENDERER, then patch the tier gate inside the
# installed Minecraft base.apk (assets/assets/tiers.bin) IN PLACE — set every
# device tier to 5 and inject the correct tier-hash for the running GPU so
# RenderDragon's lookup matches. Metadata (owner/mode/mtime/SELinux) is
# preserved so PackageManager does not re-verify the signature on next boot.
# Idempotent: a size+mtime stamp skips work unless Minecraft was updated.

MODDIR=${0%/*}
PKG=com.mojang.minecraftpe
LOG=$MODDIR/vvunlock.log
: > "$LOG"
log(){ echo "$(date 2>/dev/null) $*" >> "$LOG"; }

log "mcpe_vv service start"

# Wait (bounded) for PackageManager + SurfaceFlinger to be ready.
APK=""; R=""
i=0
while [ $i -lt 90 ]; do
  APK=$(pm path "$PKG" 2>/dev/null | sed -n 's/^package://p' | head -1)
  R=$(dumpsys SurfaceFlinger 2>/dev/null | grep -m1 "GLES:" \
        | sed "s/^.*GLES: //; s/^[^,]*, //; s/, OpenGL ES.*//")
  [ -n "$APK" ] && [ -n "$R" ] && break
  sleep 2; i=$((i+1))
done

[ -z "$APK" ] && { log "Minecraft not installed — nothing to do"; exit 0; }
[ -z "$R" ]   && { log "could not detect GL_RENDERER — aborting"; exit 0; }
log "apk=$APK"
log "GL_RENDERER=$R"

# Skip if this exact apk was already patched.
SIG="$(stat -c '%s %Y' "$APK" 2>/dev/null)"
if [ -f "$MODDIR/patched.stamp" ] && [ "$(cat "$MODDIR/patched.stamp")" = "$SIG" ]; then
  log "already patched (stamp match) — skipping"; exit 0
fi

# Capture metadata to restore after the swap.
OWN=$(stat -c '%u:%g' "$APK" 2>/dev/null)
MODE=$(stat -c '%a' "$APK" 2>/dev/null)
CTX=$(ls -Zd "$APK" 2>/dev/null | awk '{print $1}')
touch -r "$APK" "$MODDIR/.mtimeref" 2>/dev/null

NEW="$APK.vvnew"
rm -f "$NEW"
if "$MODDIR/vvpatch" "$APK" "$NEW" "$R" >>"$LOG" 2>&1 && [ -s "$NEW" ]; then
  [ -n "$OWN" ]  && chown "$OWN" "$NEW" 2>/dev/null
  [ -n "$MODE" ] && chmod "$MODE" "$NEW" 2>/dev/null
  touch -r "$MODDIR/.mtimeref" "$NEW" 2>/dev/null
  [ -n "$CTX" ]  && chcon "$CTX" "$NEW" 2>/dev/null
  if mv -f "$NEW" "$APK"; then
    stat -c '%s %Y' "$APK" > "$MODDIR/patched.stamp"
    log "patched OK — Vibrant Visuals unlocked"
  else
    log "swap (mv) FAILED"; rm -f "$NEW"
  fi
else
  log "vvpatch FAILED (see above)"; rm -f "$NEW"
fi
rm -f "$MODDIR/.mtimeref"
