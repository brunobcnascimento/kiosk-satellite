import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:kiosk_satellite/managers/voice/assist_view.dart';
import 'package:kiosk_satellite/ui/assist/assist_panels.dart';
import 'package:kiosk_satellite/ui/assist/assist_skins.dart';

void main() {
  const results = [
    AssistResult('weather', {
      'current_temperature': '72 °F',
      'current_humidity': '40%',
      'forecast_type': 'daily',
      'forecast': [
        {'date': '2026-09-27', 'condition': 'sunny', 'temperature': 75},
        {'date': '2026-09-28', 'condition': 'rainy', 'temperature': 64},
      ],
    }),
    AssistResult('weather', {
      'forecast_type': 'twice_daily',
      'forecast': [
        {'date': '2026-09-27', 'condition': 'cloudy', 'is_daytime': false},
      ],
    }),
    AssistResult('financial', {
      'query_type': 'stock',
      'name': 'Apple',
      'symbol': 'AAPL',
      'exchange': 'NASDAQ - Real Time',
      'current_price': 231.5,
      'change': -1.25,
      'percent_change': -0.54,
      'open': 232,
      'high': 233.1,
      'low': 230.2,
    }),
    AssistResult('financial', {
      'query_type': 'crypto',
      'name': 'Bitcoin',
      'current_price': 0.5,
      'change': 0.01,
      'market_cap': 1.2e12,
    }),
    AssistResult('financial', {
      'query_type': 'currency',
      'amount': 100,
      'from_currency': 'USD',
      'converted_amount': 92.1,
      'to_currency': 'EUR',
      'rate': 0.921,
    }),
    AssistResult('images', {
      'items': [
        {'image_url': 'http://x/1.jpg'},
        {'image_url': 'http://x/2.jpg', 'thumbnail_url': 'http://x/2s.jpg'},
        {'image_url': 'http://x/3.jpg'},
      ],
    }),
    AssistResult('featured', {'image_url': '/local/a.png'}),
    AssistResult('videos', {
      'items': [
        {
          'video_id': 'abc',
          'title': 'Cats',
          'channel': 'Cat TV',
          'duration': '3:12',
          'thumbnail_url': 'http://x/t.jpg',
        },
      ],
    }),
  ];

  // Every panel in every skin, light and dark, landscape and portrait:
  // a layout that throws leaves the result blank with only a log line.
  for (final skin in assistSkins) {
    testWidgets('${skin.name} panels lay out', (tester) async {
      for (final size in const [Size(1280, 800), Size(800, 1280)]) {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        for (final dark in [false, true]) {
          for (final result in results) {
            await tester.pumpWidget(
              MaterialApp(
                home: Scaffold(
                  body: Center(
                    child: AssistResultPanel(
                      result: result,
                      skin: skin,
                      dark: dark,
                      scale: 1.5,
                      resolve: (u) => u,
                      onOpen: (_, _) {},
                    ),
                  ),
                ),
              ),
            );
            await tester.pump();
            expect(tester.takeException(), isNull, reason: result.kind);
          }
        }
      }
      tester.view.reset();
    });
  }

  // Voice Satellite LLM Tools serves its condition icons as SVG, which
  // Image.network drops without a word (issue #1182).
  testWidgets('the weather icon draws SVG and raster images', (tester) async {
    Widget panel(String icon) => MaterialApp(
      home: Scaffold(
        body: AssistResultPanel(
          result: AssistResult('weather', {
            'condition_icon': icon,
            'current_temperature': '72 °F',
          }),
          skin: assistSkins.first,
          dark: false,
          scale: 1,
          resolve: (u) => 'http://ha$u',
          onOpen: (_, _) {},
        ),
      ),
    );
    const svg =
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">'
        '<circle cx="24" cy="24" r="10" fill="#FFC107"/></svg>';
    final requests = <String>[];
    await tester.runAsync(
      () => http.runWithClient(
        () async {
          await tester.pumpWidget(
            panel('/api/voice_satellite_llm_tools/weather_icons/clear_day.svg'),
          );
          for (var i = 0; i < 20 && requests.isEmpty; i++) {
            await Future<void>.delayed(const Duration(milliseconds: 10));
          }
          await Future<void>.delayed(const Duration(milliseconds: 100));
        },
        () => MockClient((request) async {
          requests.add('${request.url}');
          return http.Response(svg, 200);
        }),
      ),
    );
    await tester.pump();
    expect(requests, [
      'http://ha/api/voice_satellite_llm_tools/weather_icons/clear_day.svg',
    ]);
    expect(find.byType(SvgPicture), findsOneWidget);
    // The decoded picture, not the placeholder or the error builder.
    expect(
      find.byWidgetPredicate(
        (w) => '${w.runtimeType}'.contains('RawPictureVectorGraphic'),
      ),
      findsOneWidget,
    );
    expect(find.byType(Image), findsNothing);

    await tester.pumpWidget(panel('/local/sunny.png'));
    expect(find.byType(SvgPicture), findsNothing);
    expect(find.byType(Image), findsOneWidget);
  });

  test('the panel picks what Voice Satellite shows first', () {
    expect(primaryResult(results)!.kind, 'videos');
    expect(primaryResult(results.take(5).toList())!.kind, 'weather');
    expect(primaryResult(const []), isNull);
  });

  test('prices read as Voice Satellite formats them', () {
    expect(formatPrice(231.5, 'USD'), r'$231.50');
    expect(formatPrice(0.5, 'USD'), r'$0.500000');
    expect(formatChange(-1.25, -0.54, 'USD'), r'-$1.25 (-0.54%)');
    expect(formatLargeNumber(1.2e12, 'USD'), r'$1.20T');
  });
}
