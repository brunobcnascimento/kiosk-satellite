import 'package:flutter_test/flutter_test.dart';
import 'package:kiosk_satellite/core/command_registry.dart';
import 'package:kiosk_satellite/core/event_bus.dart';
import 'package:kiosk_satellite/core/headless.dart';
import 'package:kiosk_satellite/core/logging.dart';
import 'package:kiosk_satellite/managers/settings/definitions.dart' as defs;
import 'package:kiosk_satellite/managers/settings/echo_defaults.dart';
import 'package:kiosk_satellite/managers/settings/headless_settings.dart';
import 'package:kiosk_satellite/managers/settings/settings_manager.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// The Echo images' first-start settings: the real Wi-Fi address on every
/// image and the home screen on the Shows, set once on a fresh install and
/// never on an install that already has settings, because the real
/// address gives Home Assistant a new ESPHome device.
///
/// The updater comes from the image's properties in SystemUpdateBridge.kt:
/// `ro.biscuit.ota` is `biscuit`, `ro.echo.ota` alone is `echo`, both set
/// (Dot releases 2026.10.7 to 2026.10.9) is `biscuit`, and neither is null
/// (SystemUpdateBridgeTest.kt).
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late EventBus bus;
  late Logger log;
  late SettingsManager settings;

  /// How many times the updater was asked.
  late int asked;

  Future<void> boot(Map<String, Object> prefs) async {
    SharedPreferences.setMockInitialValues(prefs);
    bus = EventBus();
    log = Logger();
    settings = SettingsManager(bus, CommandRegistry(log), log);
    await settings.init();
  }

  /// A second start on the same stored settings.
  Future<void> restart() async {
    settings = SettingsManager(bus, CommandRegistry(log), log);
    await settings.init();
  }

  Future<void> apply(String? updater) {
    asked = 0;
    return EchoDefaults(
      settings,
      log,
      updater: () async {
        asked++;
        return updater;
      },
    ).apply();
  }

  tearDown(() {
    defs.deviceHiddenKeys.clear();
    defs.deviceHiddenOptions.clear();
    Headless.debugActive = false;
  });

  group('each image', () {
    test('an Echo Show gets the real address, the home screen, the '
        'Horizontal Now Playing and no reactive bar', () async {
      await boot({});
      await apply('echo');
      expect(settings.get(defs.esphomeRealMac), isTrue);
      expect(settings.get(defs.homeLauncherEnabled), isTrue);
      expect(settings.get(defs.sendspinFullscreenLayout), 'horizontal');
      expect(settings.get(defs.voiceReactiveBar), isFalse);
      expect(settings.internal(EchoDefaults.marker), 'echo');
    });

    test('the Echo Dot gets the real address and never the home screen, '
        'also with both properties set', () async {
      await boot({});
      await apply('biscuit');
      expect(settings.get(defs.esphomeRealMac), isTrue);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
      expect(settings.get(defs.sendspinFullscreenLayout), 'vertical');
      expect(settings.get(defs.voiceReactiveBar), isTrue);
      expect(settings.internal(EchoDefaults.marker), 'biscuit');
    });

    test('the Dot stays off the home screen with its headless pins', () async {
      await boot({});
      await apply('biscuit');
      Headless.debugActive = true;
      await HeadlessSettings(settings, bus, log).init();
      expect(settings.get(defs.esphomeRealMac), isTrue);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
    });

    test('any other device gets nothing', () async {
      await boot({});
      await apply(null);
      expect(settings.get(defs.esphomeRealMac), isFalse);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
      expect(settings.get(defs.sendspinFullscreenLayout), 'vertical');
      expect(settings.get(defs.voiceReactiveBar), isTrue);
      expect(settings.internal(EchoDefaults.marker), isEmpty);
    });

    test('no native side gets nothing', () async {
      await boot({});
      await EchoDefaults(
        settings,
        log,
        updater: () async => throw StateError('no channel'),
      ).apply();
      expect(settings.get(defs.esphomeRealMac), isFalse);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
    });
  });

  group('fresh install only', () {
    test('what the native side writes before the first start still counts '
        'as fresh', () async {
      await boot({
        'ks.render.boot_pending_since': 1761400000000,
        'ks.render.webview_probe': 1,
        'ks.render.disable_impeller': false,
        'ks.crash.was_foreground': true,
        'ks.home.attempts': 1,
      });
      expect(settings.firstRun, isTrue);
      await apply('echo');
      expect(settings.get(defs.esphomeRealMac), isTrue);
      expect(settings.get(defs.homeLauncherEnabled), isTrue);
    });

    test('an upgrade with both settings off keeps them off', () async {
      await boot({
        'ks.ha.url': 'http://homeassistant.local:8123',
        'ks.voice.runtime': 'native',
      });
      expect(settings.firstRun, isFalse);
      await apply('echo');
      expect(asked, 0);
      expect(settings.get(defs.esphomeRealMac), isFalse);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
    });

    test('an upgrade that never changed a setting keeps them off', () async {
      // Every start writes the migration markers, so an install that
      // ran once has them even with all settings at their defaults.
      await boot({'ks.internal.person.sensor.migrated': '1'});
      await apply('echo');
      expect(settings.get(defs.esphomeRealMac), isFalse);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
      expect(settings.get(defs.sendspinFullscreenLayout), 'vertical');
    });

    test('a restored backup or an import keeps its own values', () async {
      await boot({
        'ks.esphome.enabled': true,
        'ks.esphome.real_mac': false,
        'ks.secret.remote_auth': 'abc',
      });
      await apply('biscuit');
      expect(settings.get(defs.esphomeRealMac), isFalse);
    });

    test('a value turned off after the first start stays off', () async {
      await boot({});
      await apply('echo');
      await settings.set(defs.esphomeRealMac, false);
      await settings.set(defs.homeLauncherEnabled, false);
      await restart();
      expect(settings.firstRun, isFalse);
      await apply('echo');
      expect(asked, 0);
      expect(settings.get(defs.esphomeRealMac), isFalse);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
    });

    test('runs once in the same start', () async {
      await boot({});
      await apply('echo');
      await settings.set(defs.homeLauncherEnabled, false);
      await apply('echo');
      expect(asked, 0);
      expect(settings.get(defs.homeLauncherEnabled), isFalse);
    });
  });
}
