'use strict';
// 配置页：两个开关拨动即保存（宿主转给原生模块，游戏里即时生效）；状态向原生模块要。
const host = window.betterEndfield;
const $ = (selector) => document.querySelector(selector);
const defaults = { hide_mask: true, official_face: true };
const phases = {
  waiting_runtime: '等游戏加载 · waiting for the game',
  ready: '运行中 · running',
  failed: '这个游戏版本对不上，没有生效 · not compatible with this game version',
};
let configuration = { ...defaults };

function showError(error) {
  const box = $('#error');
  box.hidden = !error;
  box.textContent = error ? String(error.message || error) : '';
}

function render() {
  $('#hide_mask').checked = configuration.hide_mask !== false;
  $('#official_face').checked = configuration.official_face !== false;
}

// 换头离不开隐藏面罩：打开换头时一并打开隐藏，关掉隐藏时一并关掉换头。
function linked(value, changed) {
  if (changed === 'official_face' && value.official_face) {
    return { ...value, hide_mask: true };
  }
  if (value.hide_mask === false) {
    return { ...value, official_face: false };
  }
  return value;
}

async function save(event) {
  configuration = linked({
    ...configuration,
    hide_mask: $('#hide_mask').checked,
    official_face: $('#official_face').checked,
  }, event.target.id);
  render();
  try {
    await host.saveConfig(configuration);
    showError(null);
  } catch (error) {
    showError(error);
  }
}

async function refresh() {
  try {
    const state = await host.status(); // { connected, mode: 'game' | 'offline', module }
    $('#connection').textContent = !state.connected ? '游戏未运行 · game not running'
      : state.mode === 'offline' ? '离线宿主，不在游戏里 · offline host, not in game'
      : '游戏已连接 · game connected';
    if (state.connected) {
      await host.send({ action: 'status' });
    } else {
      $('#phase').textContent = '—';
      $('#parts').textContent = '—';
    }
    showError(null);
  } catch (error) {
    showError(error);
  }
}

host?.onmessage((message) => {
  if (message.kind !== 'reply' || !message.body || message.body.phase === undefined) {
    return;
  }
  const body = message.body;
  $('#phase').textContent = (phases[body.phase] || body.phase) + ' (' + body.version + ')';
  $('#parts').textContent = body.phase !== 'ready' ? '—'
    : body.official_face_available === false ? '换头不可用，只隐藏面罩 · head swap unavailable, mask hiding only'
    : String(body.parts_swapped);
});

$('#hide_mask').addEventListener('change', save);
$('#official_face').addEventListener('change', save);
$('#refresh').addEventListener('click', refresh);

(async () => {
  if (!host) { // 直接在浏览器里打开时没有宿主
    render();
    showError('请在 Endfield Mod Loader 里打开这个页面 · Open this page from Endfield Mod Loader');
    return;
  }
  try {
    configuration = linked({ ...defaults, ...(await host.readConfig()) });
  } catch (error) {
    showError(error);
  }
  render();
  refresh();
})();
