import '../../core/logging.dart';
import '../update/system_update_source.dart';
import 'definitions.dart' as defs;
import 'settings_manager.dart';

/// The settings a fresh install starts with on the Echo images
/// (`jxlarrea/lineageos-echo-kiosk-satellite`).
///
/// The images join every network with the factory address, because
/// per-network MAC randomization is off in the image. The real address is
/// the one the router, DHCP reservations and Home Assistant's network
/// integrations already know, so the ESPHome device links with them. On
/// an Echo Show the kiosk is also the home screen: an app in the background
/// loses the microphone on Android 11, and the home screen is what comes
/// back whenever something else took the screen. Now Playing starts in the
/// Horizontal layout there, which fits the Shows' wide screens. The reactive
/// activity bar starts off, since the Shows' MT8163 cannot animate it
/// smoothly. The Dot
/// runs headless and has no home screen to be (HeadlessSettings pins it
/// off).
///
/// The image is told apart by its updater (SystemUpdateBridge.kt), where
/// the Dot's BiscuitService wins over EchoService when a Dot sets both.
///
/// Only the first start of a fresh install gets these
/// ([SettingsManager.firstRun]). Turning the real address on for a kiosk
/// Home Assistant already knows creates a new ESPHome device there, so an
/// upgrade, a restored backup, a settings import and a Fleet profile keep
/// what they had. That is also why the settings' own defaults stay off.
class EchoDefaults {
  EchoDefaults(
    this._settings,
    this._log, {
    this.updater = RomSystemUpdateSource.detect,
  });

  final SettingsManager _settings;
  final Logger _log;

  /// The ROM's updater (SystemUpdateBridge.kt). Tests swap it.
  final Future<String?> Function() updater;

  /// Internal key holding the updater the defaults were set for.
  static const marker = 'echo.defaults';

  /// The values for each updater: `biscuit` is the Echo Dot image and
  /// `echo` an Echo Show image.
  static Map<defs.SettingDef<Object>, Object> forUpdater(String? updater) =>
      switch (updater) {
        'biscuit' => {defs.esphomeRealMac: true},
        'echo' => {
          defs.esphomeRealMac: true,
          defs.homeLauncherEnabled: true,
          defs.sendspinFullscreenLayout: 'horizontal',
          defs.voiceReactiveBar: false,
        },
        _ => const {},
      };

  /// Before any manager reads its settings, so the home screen reconciles
  /// at boot and ESPHome starts with the real address.
  Future<void> apply() async {
    if (!_settings.firstRun || _settings.internal(marker).isNotEmpty) return;
    String? rom;
    try {
      rom = await updater();
    } catch (_) {
      // No native side (a test, another platform): not an Echo image.
      return;
    }
    final values = forUpdater(rom);
    if (values.isEmpty) return;
    for (final entry in values.entries) {
      await _settings.set(entry.key, entry.value, source: 'Echo image');
    }
    await _settings.setInternal(marker, rom!);
    _log.info('settings', 'set the Echo image defaults ($rom)');
  }
}
