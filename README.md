# hardware/radicales

Board-neutral vendor HALs shared by our Android 15 ports (Radxa ROCK 5, Raspberry Pi 5). Checked out at
`hardware/radicales` by the boards' local manifests.

| Path | What |
|---|---|
| `lights/ddc` | Lights HAL for HDMI/DisplayPort monitors: the backlight level goes to the monitor over DDC/CI |

## lights/ddc

External monitors have no Linux backlight device. Most accept brightness over DDC/CI instead: VESA MCCS
feature 0x10 (luminance), written over I2C address 0x37 on the display's DDC lines. This HAL
(`android.hardware.lights-service.ddc`, AIDL `android.hardware.light` V2) serves the BACKLIGHT light that
way.

- **Finding the monitor:** it lists the DRM connectors in `/sys/class/drm` and, for each connected one,
  follows its `ddc` link to the I2C bus. Only those buses are opened; a monitor must answer a DDC/CI
  luminance query to be used. Several monitors all get the same level, scaled to each one's range.
- **Writing:** a worker thread writes the latest level and drops the ones in between, so a slider drag
  turns into a few writes (each takes ~50 ms). The first level after boot is always written. After a failed
  write (unplugged monitor) it looks for monitors again, and at boot it keeps looking for a minute.
- **Level 0** (screen off) is ignored: the display output is off anyway, and a monitor left at its lowest
  level would stay dark through the next boot.

### Board integration

```make
# device.mk
PRODUCT_PACKAGES += android.hardware.lights-service.ddc

# BoardConfig.mk
BOARD_VENDOR_SEPOLICY_DIRS += hardware/radicales/lights/ddc/sepolicy
```

```
# ueventd.<board>.rc: the HAL runs as user system
/dev/i2c-*                0660   root       system

# sepolicy genfs_contexts: the board's DRM connector directories, e.g.
genfscon sysfs /devices/platform/display-subsystem/drm/card0/card0- u:object_r:sysfs_drm_connector:s0
```

The sepolicy declares `i2c_device` for `/dev/i2c-*`; a board must not declare it again.

The display controller's DDC adapter must pass plain I2C transfers to address 0x37. Some HDMI controllers'
built-in I2C masters do only EDID reads (RK3588's dw-hdmi-qp); the ROCK 5 runs the DDC pins on `i2c-gpio`
instead (see its device tree).

Check from a shell: `i2ctransfer -y <bus> w5@0x37 0x51 0x82 0x01 0x10 0xac`, then
`i2ctransfer -y <bus> r11@0x37`; bytes 7-8 of the reply are the maximum, 9-10 the current level.

## License

Apache 2.0, see [LICENSE](LICENSE).
