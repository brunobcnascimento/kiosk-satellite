import 'package:flutter_test/flutter_test.dart';
import 'package:kiosk_satellite/managers/browser/carousel_script.dart';

/// Carousel view eligibility (issue #1206): a view whose `visible` lists
/// users is shown in HA's header only to those users, so the carousel
/// must skip it for everyone else instead of treating any list as
/// visible. The script is a string baked into the page, so the contract
/// is asserted on its markers.
void main() {
  test('views are filtered with the header rule, not visible:false only', () {
    expect(dashboardCarouselScript, contains('function viewVisible(hr, v)'));
    expect(dashboardCarouselScript, contains('!viewVisible(hr, v)'));
    expect(dashboardCarouselScript, isNot(contains('v.visible === false')));
  });

  test('a user list matches the logged-in user id', () {
    expect(dashboardCarouselScript, contains('Array.isArray(v.visible)'));
    expect(dashboardCarouselScript, contains('hr.hass && hr.hass.user'));
    expect(dashboardCarouselScript, contains('v.visible[i].user === id'));
  });
}
