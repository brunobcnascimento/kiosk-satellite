import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:kiosk_satellite/app_container.dart';
import 'package:kiosk_satellite/managers/settings/definitions.dart' as defs;
import 'package:kiosk_satellite/managers/voice/assist_view.dart';
import 'package:kiosk_satellite/ui/assist/assist_overlay.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  late AppContainer c;

  setUp(() async {
    SharedPreferences.setMockInitialValues({});
    c = AppContainer();
    await c.settings.init();
  });

  tearDown(() async {
    await c.settings.dispose();
  });

  Future<void> pumpOverlay(WidgetTester tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(body: AssistOverlay(container: c)),
      ),
    );
  }

  testWidgets('an opaque skin covers the screen once it has faded in', (
    tester,
  ) async {
    await c.settings.set(defs.voiceSkin, 'google-home');
    await pumpOverlay(tester);
    expect(c.voice.covers.value, isFalse);

    c.voice.view.value = const AssistView(phase: AssistPhase.listening);
    await tester.pump();
    // Mid fade the screen still shows through.
    await tester.pump(const Duration(milliseconds: 100));
    expect(c.voice.covers.value, isFalse);
    await tester.pump(const Duration(milliseconds: 300));
    expect(c.voice.covers.value, isTrue);

    // The report that it no longer covers goes out as the hide starts,
    // not when the fade out ends.
    c.voice.view.value = AssistView.hidden;
    await tester.pump();
    expect(c.voice.covers.value, isFalse);
    await tester.pump(const Duration(milliseconds: 500));
    expect(c.voice.covers.value, isFalse);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('a docked bubble and a see-through backdrop do not cover', (
    tester,
  ) async {
    await c.settings.set(defs.voiceSkin, 'google-home');
    await pumpOverlay(tester);

    c.voice.view.value = const AssistView(
      phase: AssistPhase.listening,
      docked: true,
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(c.voice.covers.value, isFalse);
    c.voice.view.value = AssistView.hidden;
    await tester.pump(const Duration(milliseconds: 500));

    // A blurred skin shows the screen through its backdrop until the
    // blurred capture lands, which the test harness never provides.
    await c.settings.set(defs.voiceSkin, 'default');
    c.voice.view.value = const AssistView(phase: AssistPhase.listening);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(c.voice.covers.value, isFalse);

    // Lowering an opaque skin's backdrop opacity lets the screen through.
    await c.settings.set(defs.voiceSkin, 'google-home');
    await c.settings.set(defs.voiceBackgroundOpacity, 60);
    await tester.pump();
    expect(c.voice.covers.value, isFalse);
    await c.settings.set(defs.voiceBackgroundOpacity, 100);
    await tester.pump();
    expect(c.voice.covers.value, isTrue);
    c.voice.view.value = AssistView.hidden;
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
