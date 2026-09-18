/* app.js — 门禁上位机前端逻辑(零依赖,单文件)
 *
 * 结构:状态 → 工具(fetch/提示/数字动画) → 登录 → 概览 → 实时事件(WS)
 *       → 记录查询 → 时间同步 → 账号安全 → 启动流程。
 * 纪律:
 *   - 所有接口调用经 api():统一带 token、统一处理 401(掉线回登录页)
 *   - WebSocket 断线自动重连(退避),状态在顶栏可见
 *   - 时间/姓名等文本一律用 textContent 或转义写入,不用 innerHTML 拼数据
 *     (用户姓名是用户可控字段,拼 HTML 就是自造 XSS)
 */
'use strict';

const $ = (id) => document.getElementById(id);
const TOKEN_KEY = 'dg_token';

const S = {
  token: sessionStorage.getItem(TOKEN_KEY) || '',
  user: '',
  page: 1,
  pages: 1,
  ws: null,
  wsTries: 0,
  ntpPending: false,
};

/* ---------------- 工具 ---------------- */

function toast(msg, kind) {
  const box = document.createElement('div');
  box.className = 'toast ' + (kind || '');
  box.textContent = msg;
  $('toasts').appendChild(box);
  setTimeout(() => {
    box.classList.add('out');
    setTimeout(() => box.remove(), 300);
  }, kind === 'err' ? 5200 : 3200);
}

function esc(s) {
  return String(s == null ? '' : s);
}

/* 数字滚动:只对变化的数值生效,避免每次刷新都跳 */
function countUp(el, to) {
  const from = Number(el.dataset.count || 0);
  if (from === to) { el.textContent = to; return; }
  el.dataset.count = to;
  const steps = 18;
  let i = 0;
  const timer = setInterval(() => {
    i++;
    const v = Math.round(from + (to - from) * (i / steps));
    el.textContent = v;
    if (i >= steps) clearInterval(timer);
  }, 24);
}

function fmtBytes(n) {
  if (!n) return '—';
  const u = ['B', 'KB', 'MB', 'GB'];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return v.toFixed(v < 10 && i > 0 ? 1 : 0) + ' ' + u[i];
}

async function api(path, opts) {
  opts = opts || {};
  const headers = Object.assign({}, opts.headers || {});
  if (S.token) headers['X-Auth-Token'] = S.token;
  if (opts.body) headers['Content-Type'] = 'application/json';
  const res = await fetch(path, Object.assign({}, opts, { headers }));
  let data = null;
  try { data = await res.json(); } catch (e) { data = null; }
  if (res.status === 401) {
    logout(false);
    showLogin((data && data.msg) || '会话已过期,请重新登录');
    throw new Error('unauthorized');
  }
  if (!res.ok) {
    const msg = (data && data.msg) || ('请求失败(' + res.status + ')');
    throw new Error(msg);
  }
  return data || {};
}

/* 按钮水波纹(纯装饰:pointerdown 时插一个扩散圆) */
document.addEventListener('pointerdown', (e) => {
  const btn = e.target.closest('.btn');
  if (!btn) return;
  const r = btn.getBoundingClientRect();
  const span = document.createElement('span');
  const size = Math.max(r.width, r.height);
  span.className = 'ripple';
  span.style.width = span.style.height = size + 'px';
  span.style.left = (e.clientX - r.left - size / 2) + 'px';
  span.style.top = (e.clientY - r.top - size / 2) + 'px';
  btn.appendChild(span);
  setTimeout(() => span.remove(), 600);
});

/* ---------------- 登录/登出 ---------------- */

function showLogin(msg) {
  $('app-view').hidden = true;
  $('login-view').style.display = '';
  if (msg) {
    $('login-msg').textContent = msg;
    const card = $('login-form');
    card.classList.remove('shake');
    void card.offsetWidth;                 /* 强制重排:同一动画可重复触发 */
    card.classList.add('shake');
  }
  $('login-pwd').focus();
}

function logout(notify) {
  if (S.token) {
    const t = S.token;
    S.token = '';
    sessionStorage.removeItem(TOKEN_KEY);
    fetch('/api/logout', { method: 'POST', headers: { 'X-Auth-Token': t } })
      .catch(() => {});
  }
  if (S.ws) { try { S.ws.close(); } catch (e) {} S.ws = null; }
  if (notify) toast('已退出登录');
}

$('login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const btn = $('login-btn');
  btn.disabled = true;
  $('login-msg').textContent = '';
  try {
    const r = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ user: $('login-user').value, pwd: $('login-pwd').value }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.msg || '登录失败');
    S.token = j.token;
    S.user = j.user || '';
    sessionStorage.setItem(TOKEN_KEY, S.token);
    $('login-pwd').value = '';
    $('pwd-banner').hidden = !j.pwd_default;
    boot();
    toast('登录成功,欢迎 ' + esc(S.user), 'ok');
  } catch (err) {
    $('login-msg').textContent = err.message;
    const card = $('login-form');
    card.classList.remove('shake');
    void card.offsetWidth;
    card.classList.add('shake');
  } finally {
    btn.disabled = false;
  }
});

$('btn-logout').addEventListener('click', () => { logout(true); showLogin(''); });

/* ---------------- 设备概览 ---------------- */

async function loadDevice() {
  const d = await api('/api/device');
  $('st-version').textContent = d.version || '—';
  $('st-uptime').textContent = d.uptime_text || (d.uptime_s + 's');
  countUp($('st-users'), d.users || 0);
  countUp($('st-logs'), d.log_total || 0);
  $('st-addr').textContent = d.mdns_running
    ? 'http://' + d.mdns_host + '.local:' + d.web_port + '  ·  ' +
      (d.ip ? d.ip + ':' + d.web_port : '无 IP')
    : (d.ip ? 'http://' + d.ip + ':' + d.web_port : '设备未联网');
  $('st-disk').textContent = '库 ' + fmtBytes(d.db_bytes) + ' / 余 ' + fmtBytes(d.disk_free_bytes);
  $('foot-ver').textContent = '固件 ' + (d.version || '—');
  if (d.ntp && d.ntp.ok && d.ntp.last_ok_at) {
    $('ntp-state').textContent = '上次校正成功:' + d.ntp.last_ok_at;
  }
  S.online = !!d.online;
  syncNtpBtn();
  S.user = d.web_user || S.user;
  $('who').textContent = S.user ? ('管理员 ' + S.user) : '';
  $('ac-user').value = S.user;
  $('pwd-banner').hidden = !d.pwd_default;
  return d;
}

/* ---------------- 实时事件(WebSocket) ---------------- */

function setConn(on, text) {
  const el = $('conn');
  el.classList.toggle('on', !!on);
  el.classList.toggle('off', !on);
  $('conn-text').textContent = text;
  $('live-badge').hidden = !on;
}

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const url = proto + location.host + '/api/ws?token=' + encodeURIComponent(S.token);
  let ws;
  try { ws = new WebSocket(url); } catch (e) { setConn(false, '不支持'); return; }
  S.ws = ws;
  ws.onopen = () => { S.wsTries = 0; setConn(true, '实时连接'); };
  ws.onclose = () => {
    setConn(false, '已断开');
    if (!S.token) return;                   /* 主动退出:不重连 */
    S.wsTries++;
    const delay = Math.min(15000, 800 * Math.pow(1.6, S.wsTries));
    setConn(false, '重连中…');
    /* 连不上多半是服务端会话已失效(重启/改密):探一次接口,401 会
     * 走 api() 的统一处理回登录页,而不是让用户一直看"重连中" */
    if (S.wsTries === 2 && S.token) {
      api('/api/device').catch(() => {});
    }
    setTimeout(connectWS, delay);
  };
  ws.onerror = () => setConn(false, '连接异常');
  ws.onmessage = (ev) => {
    let j;
    try { j = JSON.parse(ev.data); } catch (e) { return; }
    if (j.type === 'auth') addEvent(j);
    else if (j.type === 'ntp') {
      S.ntpPending = false;
      syncNtpBtn();
      if (j.ok) $('ntp-state').textContent = '上次校正成功:' + j.time;
      toast(j.msg || (j.ok ? '时间校正成功' : '时间校正失败'), j.ok ? 'ok' : 'err');
      loadDevice().catch(() => {});
    }
  };
}

function addEvent(j) {
  const feed = $('feed');
  const ph = feed.querySelector('.empty');
  if (ph) ph.remove();

  const pass = Number(j.result) === 0;
  const row = document.createElement('div');
  row.className = 'ev ' + (pass ? 'ok' : 'err');

  const t = document.createElement('span');
  t.className = 't';
  t.textContent = j.time || '';
  const n = document.createElement('span');
  n.className = 'n';
  n.textContent = j.user_name || '陌生人';
  const m = document.createElement('span');
  m.className = 'muted small';
  m.textContent = (j.user_id ? 'ID ' + j.user_id + ' · ' : '') + (j.method_name || '');
  const p = document.createElement('span');
  p.className = 'pill';
  p.textContent = pass ? '通过' : '拒绝';

  row.append(t, n, m, p);
  feed.insertBefore(row, feed.firstChild);
  while (feed.children.length > 60) feed.removeChild(feed.lastChild);
  countUp($('st-logs'), Number($('st-logs').dataset.count || 0) + 1);
}

$('btn-clear-feed').addEventListener('click', () => {
  $('feed').innerHTML = '<p class="empty">等待验证事件…</p>';
});

/* ---------------- 记录查询 ---------------- */

const dateStr = (d) => d.toISOString().slice(0, 10);

document.querySelectorAll('.chip[data-range]').forEach((chip) => {
  chip.addEventListener('click', () => {
    document.querySelectorAll('.chip[data-range]').forEach((c) => c.classList.remove('on'));
    chip.classList.add('on');
    const r = chip.dataset.range;
    const today = new Date();
    if (r === 'today') {
      $('q-from').value = dateStr(today);
      $('q-to').value = dateStr(today);
    } else if (r === '7d') {
      const from = new Date(today.getTime() - 6 * 86400000);
      $('q-from').value = dateStr(from);
      $('q-to').value = dateStr(today);
    } else {
      $('q-from').value = '';
      $('q-to').value = '';
    }
    loadLogs(1);
  });
});

async function loadLogs(page) {
  S.page = page || S.page || 1;
  const body = $('logs-body');
  body.innerHTML = '<tr><td colspan="5" class="empty">查询中…</td></tr>';
  const q = new URLSearchParams();
  if ($('q-from').value) q.set('from', $('q-from').value);
  if ($('q-to').value) q.set('to', $('q-to').value);
  if ($('q-uid').value.trim()) q.set('user_id', $('q-uid').value.trim());
  q.set('page', S.page);
  q.set('page_size', $('q-size').value);

  try {
    const d = await api('/api/logs?' + q.toString());
    S.pages = d.pages || 1;
    body.innerHTML = '';
    if (!d.logs || !d.logs.length) {
      body.innerHTML = '<tr><td colspan="5" class="empty">该条件下没有记录</td></tr>';
    } else {
      d.logs.forEach((l, idx) => {
        const tr = document.createElement('tr');
        tr.className = 'row-anim';
        tr.style.animationDelay = (idx * 18) + 'ms';
        [l.time, l.user_id || '—', l.user_name || '陌生人', l.method_name].forEach((v) => {
          const td = document.createElement('td');
          td.textContent = v;
          tr.appendChild(td);
        });
        const td = document.createElement('td');
        const sp = document.createElement('span');
        const pass = Number(l.result) === 0;
        sp.className = 'res ' + (pass ? 'pass' : 'deny');
        sp.textContent = pass ? '通过' : '拒绝';
        td.appendChild(sp);
        tr.appendChild(td);
        body.appendChild(tr);
      });
    }
    $('pg-info').textContent = '第 ' + d.page + ' / ' + (d.pages || 1) +
      ' 页 · 共 ' + d.total + ' 条';
    $('pg-prev').disabled = d.page <= 1;
    $('pg-next').disabled = d.page >= (d.pages || 1);
  } catch (err) {
    body.innerHTML = '<tr><td colspan="5" class="empty">查询失败:' + esc(err.message) + '</td></tr>';
  }
}

$('btn-query').addEventListener('click', () => loadLogs(1));
$('pg-prev').addEventListener('click', () => loadLogs(Math.max(1, S.page - 1)));
$('pg-next').addEventListener('click', () => loadLogs(Math.min(S.pages, S.page + 1)));
$('q-uid').addEventListener('keydown', (e) => { if (e.key === 'Enter') loadLogs(1); });

/* ---------------- 时间同步 ---------------- */

/* NTP 按钮可用性:未联网(或正在校正)就置灰,并说明原因 */
function syncNtpBtn() {
  const btn = $('btn-ntp');
  btn.disabled = S.ntpPending || !S.online;
  if (!S.online) $('ntp-state').textContent = '设备未联网,时间校正不可用';
}

$('btn-ntp').addEventListener('click', async () => {
  if (S.ntpPending) return;
  S.ntpPending = true;
  $('btn-ntp').disabled = true;
  try {
    await api('/api/ntp', { method: 'POST' });
    $('ntp-state').textContent = '校正中…(约需 5~15 秒)';
    toast('已发起时间校正,结果稍后提示');
  } catch (err) {
    S.ntpPending = false;
    syncNtpBtn();
    toast(err.message, 'err');
  }
});

setInterval(() => {
  const d = new Date();
  $('clock').textContent = [d.getHours(), d.getMinutes(), d.getSeconds()]
    .map((v) => String(v).padStart(2, '0')).join(':');
}, 1000);

/* ---------------- 账号安全 ---------------- */

$('btn-save-account').addEventListener('click', async () => {
  const user = $('ac-user').value.trim();
  const oldPwd = $('ac-old').value;
  const pwd = $('ac-new').value;
  const confirm = $('ac-confirm').value;
  if (pwd !== confirm) { toast('两次输入的新口令不一致', 'err'); return; }
  if (!oldPwd) { toast('请先输入原口令', 'err'); return; }
  try {
    const r = await api('/api/account', {
      method: 'POST',
      body: JSON.stringify({ user: user, old_pwd: oldPwd, pwd: pwd }),
    });
    toast(r.msg || '已保存', 'ok');
    $('ac-old').value = $('ac-new').value = $('ac-confirm').value = '';
    logout(false);
    showLogin('凭据已修改,请用新账号口令登录');
  } catch (err) {
    toast(err.message, 'err');
  }
});

$('btn-goto-account').addEventListener('click', () => {
  $('ac-user').focus();
  $('ac-user').scrollIntoView({ behavior: 'smooth', block: 'center' });
});
$('pwd-banner').hidden = true;

/* ---------------- 启动 ---------------- */

function boot() {
  $('login-view').style.display = 'none';
  $('app-view').hidden = false;
  loadDevice().catch((e) => toast(e.message, 'err'));
  loadLogs(1);
  connectWS();
  setInterval(() => { loadDevice().catch(() => {}); }, 30000);
}

if (S.token) {
  boot();
} else {
  showLogin('');
}
