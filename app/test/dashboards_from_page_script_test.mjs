import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { runInNewContext } from 'node:vm';

const source = readFileSync(new URL(
  '../lib/managers/home_assistant/home_assistant_manager.dart', import.meta.url,
), 'utf8');
const script = source.match(
  /\(function \(\) \{\n  try \{\n    var el = document\.querySelector\('home-assistant'\);\n    var hass = el && el\.hass;\n    if \(!hass \|\| !hass\.panels\) return 'null';[\s\S]*?\}\)\(\)/,
)[0];

const panelTitles = { 'panel.energy': 'Energy', 'panel.logbook': 'Activity', 'panel.media_browser': 'Media' };

function list(panels, { defaultPanel } = {}) {
  const hass = { panels, defaultPanel, localize: (key) => panelTitles[key] || '' };
  const document = { querySelector: () => ({ hass }) };
  return JSON.parse(runInNewContext(script, { document }));
}

// The panels a Home Assistant 2026.10 instance reports, trimmed.
const panels = {
  config: { url_path: 'config', component_name: 'config', title: 'config', icon: 'mdi:cog', require_admin: true },
  lovelace: { url_path: 'lovelace', component_name: 'lovelace', title: 'Overview', icon: 'mdi:view-dashboard', config: { mode: 'storage' } },
  app: { url_path: 'app', component_name: 'app', title: null, icon: null, config: null },
  '1e25efb6_panalume': { url_path: '1e25efb6_panalume', component_name: 'app', title: 'Panalume', icon: 'mdi:greenhouse', config: { addon: '1e25efb6_panalume' } },
  'home-tablet': { url_path: 'home-tablet', component_name: 'lovelace', title: 'Home Tablet', icon: 'mdi:view-dashboard', config: { mode: 'storage' } },
  hacs: { url_path: 'hacs', component_name: 'custom', title: 'HACS', icon: 'hacs:hacs', config: { _panel_custom: { name: 'hacs-frontend' } }, require_admin: true },
  a0d7b954_nodered: { url_path: 'a0d7b954_nodered', component_name: 'app', title: 'Node-RED', icon: 'mdi:sitemap', config: { addon: 'a0d7b954_nodered' } },
};

// Home Assistant's own panels from the same instance, trimmed.
const pagePanels = {
  energy: { url_path: 'energy', component_name: 'energy', title: 'energy', icon: 'mdi:lightning-bolt', config: null },
  profile: { url_path: 'profile', component_name: 'profile', title: null, icon: null, config: null },
  logbook: { url_path: 'logbook', component_name: 'logbook', title: 'logbook', icon: 'mdi:format-list-bulleted-type', config: null },
  _my_redirect: { url_path: '_my_redirect', component_name: 'my', title: null, icon: null, config: null },
  'developer-tools': { url_path: 'developer-tools', component_name: 'developer-tools', title: 'developer_tools', icon: 'mdi:hammer', config: null, require_admin: true },
  'voice-satellite': { url_path: 'voice-satellite', component_name: 'custom', title: 'Voice Satellite', icon: 'mdi:microphone-message', config: { _panel_custom: { name: 'voice-satellite-panel' } } },
  'media-browser': { url_path: 'media-browser', component_name: 'media-browser', title: 'media_browser', icon: 'mdi:play-box-multiple', config: null },
  notfound: { url_path: 'notfound', component_name: 'notfound', title: null, icon: null, config: null },
};

test('lists app panels after the dashboards', () => {
  assert.deepEqual(list(panels), [
    { url_path: 'lovelace', title: 'Overview', icon: 'mdi:view-dashboard' },
    { url_path: 'home-tablet', title: 'Home Tablet', icon: 'mdi:view-dashboard' },
    { url_path: '1e25efb6_panalume', title: 'Panalume', icon: 'mdi:greenhouse', app: true },
    { url_path: 'a0d7b954_nodered', title: 'Node-RED', icon: 'mdi:sitemap', app: true },
  ]);
});

test('skips the app store panel and admin panels', () => {
  const paths = list(panels).map((d) => d.url_path);
  assert.ok(!paths.includes('app'));
  assert.ok(!paths.includes('hacs'));
  assert.ok(!paths.includes('config'));
});

test('lists Home Assistant\'s own pages after the apps, in its language', () => {
  const pages = list({ ...pagePanels, ...panels }).filter((d) => d.builtIn);
  assert.deepEqual(pages, [
    { url_path: 'energy', title: 'Energy', icon: 'mdi:lightning-bolt', builtIn: true },
    { url_path: 'logbook', title: 'Activity', icon: 'mdi:format-list-bulleted-type', builtIn: true },
    { url_path: 'voice-satellite', title: 'Voice Satellite', icon: 'mdi:microphone-message', builtIn: true },
    { url_path: 'media-browser', title: 'Media', icon: 'mdi:play-box-multiple', builtIn: true },
  ]);
  const all = list({ ...pagePanels, ...panels });
  assert.equal(all.findIndex((d) => d.builtIn), all.length - pages.length);
});

test('skips the profile, the My redirect, the not-found page and admin pages', () => {
  const paths = list(pagePanels).map((d) => d.url_path);
  for (const path of ['profile', '_my_redirect', 'notfound', 'developer-tools']) {
    assert.ok(!paths.includes(path), path);
  }
});

test('a built-in page set as the default panel comes first', () => {
  assert.equal(list({ ...panels, ...pagePanels }, { defaultPanel: 'energy' })[0].url_path, 'energy');
});

test('names an app without a title by its path and gives it an icon', () => {
  const [app] = list({
    x_app: { url_path: 'x_app', component_name: 'app', title: null, icon: null, config: { addon: 'x_app' } },
  });
  assert.deepEqual(app, { url_path: 'x_app', title: 'x_app', icon: 'mdi:puzzle', app: true });
});

test('an app set as the default panel comes first', () => {
  assert.equal(list(panels, { defaultPanel: '1e25efb6_panalume' })[0].url_path, '1e25efb6_panalume');
});
