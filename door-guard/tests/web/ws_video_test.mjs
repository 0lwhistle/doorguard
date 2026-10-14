/*
 * ws_video_test.mjs — web 监控推流 WS 探针(node ≥22 原生 WebSocket,无依赖)
 *
 * 用法:node ws_video_test.mjs [host] [port]     (默认 192.168.137.100:80)
 * 流程:admin/admin 登录 → WS ?token= → 发 video_on → 统计 3s 内二进制 JPEG 帧。
 * 判定分两段:①连接后 2s 不订阅,应零帧("平时不跑",用户拍板);
 * ②发 video_on 后 3s 内帧数 ∈ [5,34](10fps×3s≈30 名义值,下限防慢机)。
 * 注意:真机画面内容(门口实景)只能人眼确认,本探针只验链路与节流。
 */
const host = process.argv[2] || '192.168.137.100'
const port = process.argv[3] || '80'
const user = process.env.DG_WEB_USER || 'admin'
const pwd = process.env.DG_WEB_PWD || 'admin'

const login = await fetch(`http://${host}:${port}/api/login`, {
  method: 'POST',
  headers: { 'content-type': 'application/json' },
  body: JSON.stringify({ user, pwd }),
})
if (!login.ok) {
  console.error('登录失败 HTTP', login.status, '(密码非默认?用 DG_WEB_USER/DG_WEB_PWD)')
  process.exit(2)
}
const { token } = await login.json()

const ws = new WebSocket(`ws://${host}:${port}/api/ws?token=${encodeURIComponent(token)}`)
let frames = 0
let ctl = null
const t0 = Date.now()

ws.onopen = () => {
  /* 阶段①:连接但先不订阅——推流线程应停车,2s 零帧 */
  setTimeout(() => {
    if (frames === 0) console.log('未订阅 2s 零帧 ✓(平时不跑)')
    else console.log(`未订阅却有 ${frames} 帧 ✗`)
    ws.send('{"type":"video_on"}')
  }, 2000)
}
ws.onmessage = async (ev) => {
  if (ev.data instanceof Blob) {
    frames++
    if (frames === 1) {
      const b = new Uint8Array(await ev.data.arrayBuffer())
      console.log(
        `首帧 ${b.length}B SOI=${b[0] === 0xff && b[1] === 0xd8 ? '是' : '否'}`,
      )
      if (process.env.DG_SAVE_FRAME)
        await import('node:fs/promises').then((fs) =>
          fs.writeFile(process.env.DG_SAVE_FRAME, b),
        )
    }
  } else {
    const m = JSON.parse(ev.data)
    if (m.type === 'video') {
      ctl = m
      console.log('订阅回执', JSON.stringify(m))
    }
  }
}
ws.onerror = () => console.error('WS 错误(连接被拒/会话失效?)')

setTimeout(() => {
  /* 阶段②:video_on 发出后 3s(总窗 5s,扣除 2s 空闲段)统计帧数 */
  const sec = (Date.now() - t0) / 1000 - 2
  const fps = frames / sec
  console.log(`订阅后 3s 收到 ${frames} 帧(≈${fps.toFixed(1)} fps)`)
  const ok = idleOk && ctl && ctl.ok && frames >= 5 && frames <= 34
  console.log(ok ? 'WS VIDEO PASS' : 'WS VIDEO FAIL')
  try {
    ws.close()
  } catch {
    /* 忽略 */
  }
  process.exit(ok ? 0 : 1)
}, 5000)

let idleOk = false
const _idleTimer = setTimeout(() => {
  idleOk = frames === 0
}, 1950)
