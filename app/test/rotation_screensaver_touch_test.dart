import 'dart:async';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kiosk_satellite/core/command_registry.dart';
import 'package:kiosk_satellite/core/event_bus.dart';
import 'package:kiosk_satellite/core/events.dart';
import 'package:kiosk_satellite/core/logging.dart';
import 'package:kiosk_satellite/managers/browser/browser_manager.dart';
import 'package:kiosk_satellite/managers/home_assistant/home_assistant_manager.dart';
import 'package:kiosk_satellite/managers/settings/settings_manager.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Touches under the screensaver (a Now Playing control press) must not run
/// the rotation touch pause: the ring is already frozen while the
/// screensaver is up, so every press only churned timers and logged a
/// pause for a view nobody could see. The touch that dismisses the
/// screensaver still earns the grace window, applied once the dashboard is
/// back.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late EventBus bus;
  late CommandRegistry commands;
  late Logger log;

  Future<void> build() async {
    SharedPreferences.setMockInitialValues({
      'ks.ha.url': 'http://ha.test:8123',
      'ks.ha.token': 'token',
      'ks.browser.start_url': 'http://ha.test:8123/lovelace/home',
      'ks.ha.rotation_enabled': true,
      'ks.ha.rotation_dashboards': '["lovelace/kitchen","lovelace/bath"]',
      'ks.ha.rotation_seconds': 5,
      'ks.ha.rotation_pause_seconds': 30,
    });
    bus = EventBus();
    log = Logger();
    commands = CommandRegistry(log);
    final settings = SettingsManager(bus, commands, log);
    await settings.init();
    await BrowserManager(bus, commands, log, settings).init();
    await HomeAssistantManager(bus, commands, log, settings).init();
  }

  Iterable<String> navigations() => log.recent
      .where((e) => e.tag == 'command')
      .map((e) => e.message)
      .where((m) => m.startsWith('evalJs') && m.contains('history.pushState'));

  Iterable<String> rotationLog() =>
      log.recent.where((e) => e.tag == 'home_assistant').map((e) => e.message);

  test('touches under the screensaver do not pause rotation', () {
    fakeAsync((async) {
      unawaited(build());
      async.flushMicrotasks();
      bus.publish(
        const ScreensaverStateChanged(active: true, nowPlaying: true),
      );
      async.flushMicrotasks();
      for (var i = 0; i < 3; i++) {
        bus.publish(const ActivityDetected(source: 'touch'));
        async.flushMicrotasks();
        async.elapse(const Duration(seconds: 2));
      }
      expect(rotationLog(), isNot(contains(startsWith('rotation paused'))));
      // The ring stays frozen in place under the screensaver either way.
      expect(navigations(), isEmpty);
    });
  });

  test('the dismissing touch earns the pause once the screensaver ends', () {
    fakeAsync((async) {
      unawaited(build());
      async.flushMicrotasks();
      bus.publish(const ScreensaverStateChanged(active: true));
      async.flushMicrotasks();
      bus.publish(const ActivityDetected(source: 'touch'));
      async.flushMicrotasks();
      expect(rotationLog(), isNot(contains(startsWith('rotation paused'))));
      bus.publish(const ScreensaverStateChanged(active: false));
      async.flushMicrotasks();
      expect(rotationLog(), contains('rotation paused by touch (30s)'));
      // Well past the 5 second dwell: the dismissed-to view stays up.
      async.elapse(const Duration(seconds: 29));
      expect(navigations(), isEmpty);
      async.elapse(const Duration(seconds: 1));
      expect(rotationLog(), contains('rotation resumed'));
      async.elapse(const Duration(seconds: 5));
      expect(navigations(), hasLength(1));
    });
  });

  test('a screensaver ending without a touch resumes without a pause', () {
    fakeAsync((async) {
      unawaited(build());
      async.flushMicrotasks();
      bus.publish(const ScreensaverStateChanged(active: true));
      async.flushMicrotasks();
      bus.publish(const ScreensaverStateChanged(active: false));
      async.flushMicrotasks();
      expect(rotationLog(), isNot(contains(startsWith('rotation paused'))));
      async.elapse(const Duration(seconds: 6));
      expect(navigations(), hasLength(1));
    });
  });
}
