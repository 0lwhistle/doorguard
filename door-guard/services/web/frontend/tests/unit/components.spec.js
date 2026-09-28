/*
 * components.spec.js — 组件行为(纯展示组件,用 props 驱动,不碰网络)
 */
import { describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import AppField from '../../src/components/AppField.vue'
import DataTable from '../../src/components/DataTable.vue'
import EventFeed from '../../src/components/EventFeed.vue'
import NetworkCard from '../../src/components/NetworkCard.vue'
import StatValue from '../../src/components/StatValue.vue'
import StatusPill from '../../src/components/StatusPill.vue'

describe('components/EventFeed', () => {
  const items = [
    { ts: 2, time: '10:00:02', user_name: '李四', user_id: '10002', method_name: '密码', result: 1 },
    { ts: 1, time: '10:00:01', user_name: '张三', user_id: '10001', method_name: '人脸1:N', result: 0 },
  ]

  it('渲染事件并在无数据时给等待提示', () => {
    const empty = mount(EventFeed, { props: { items: [] } })
    expect(empty.text()).toContain('等待验证事件')
    const feed = mount(EventFeed, { props: { items } })
    expect(feed.findAll('.ev')).toHaveLength(2)
    expect(feed.text()).toContain('李四')
    expect(feed.text()).toContain('ID 10001')
  })

  it('通过/拒绝用不同样式与徽章(颜色不是唯一信息来源)', () => {
    const feed = mount(EventFeed, { props: { items } })
    const rows = feed.findAll('.ev')
    expect(rows[0].classes()).toContain('ev--err')
    expect(rows[1].classes()).toContain('ev--ok')
    expect(rows[1].text()).toContain('通过')
    expect(rows[0].text()).toContain('拒绝')
  })

  it('无用户信息时显示"陌生人",且不产生 Vue 告警(列表 key 必须合法)', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const feed = mount(EventFeed, {
      props: { items: [{ ts: 3, time: '10:00:03', user_name: '', method_name: '人脸1:N', result: 1 }] },
    })
    expect(feed.text()).toContain('陌生人')
    expect(warn.mock.calls.flat().join(' ')).not.toContain('Vue warn')
  })
})

describe('components/DataTable', () => {
  const columns = [
    { key: 'a', label: 'A' },
    { key: 'b', label: 'B' },
  ]

  it('空态与加载态各有一行占位,跨全部列', () => {
    const empty = mount(DataTable, { props: { columns, rows: [], emptyText: '没有记录' } })
    expect(empty.find('td').attributes('colspan')).toBe('2')
    expect(empty.text()).toContain('没有记录')
    const loading = mount(DataTable, { props: { columns, rows: [], loading: true } })
    expect(loading.text()).toContain('查询中')
  })

  it('行数据直出,作用域插槽可自定义单元格', () => {
    const rows = [
      { id: 1, a: 'x', b: 0 },
      { id: 2, a: 'y', b: 1 },
    ]
    const table = mount(DataTable, {
      props: { columns, rows, rowKey: 'id' },
      slots: { 'cell-b': ({ row }) => (row.b === 0 ? '通过' : '拒绝') },
    })
    const body = table.findAll('tbody tr')
    expect(body).toHaveLength(2)
    expect(body[0].text()).toBe('x通过')
    expect(body[1].text()).toBe('y拒绝')
  })
})

describe('components/AppField', () => {
  it('input 受控:输入回传 update:modelValue', async () => {
    const wrapper = mount(AppField, { props: { modelValue: '', label: '账号' } })
    await wrapper.find('input').setValue('guard01')
    expect(wrapper.emitted('update:modelValue')[0]).toEqual(['guard01'])
  })

  it('options 存在时渲染 select,错误优先于提示', () => {
    const wrapper = mount(AppField, {
      props: {
        modelValue: 20,
        type: 'select',
        options: [
          { value: 20, label: '20' },
          { value: 50, label: '50' },
        ],
        hint: '提示',
        error: '错了',
      },
    })
    expect(wrapper.find('select').exists()).toBe(true)
    expect(wrapper.text()).toContain('错了')
    expect(wrapper.text()).not.toContain('提示')
  })
})

describe('components/StatValue + StatusPill', () => {
  it('文本值直接显示,不参与数字滚动', () => {
    const wrapper = mount(StatValue, { props: { value: '—', animated: false } })
    expect(wrapper.text()).toBe('—')
  })

  it('徽章变体映射到对应样式类', () => {
    expect(mount(StatusPill, { props: { kind: 'pass', label: '通过' } }).classes()).toContain('res--pass')
    expect(mount(StatusPill, { props: { kind: 'deny', label: '拒绝' } }).classes()).toContain('res--deny')
  })
})

describe('components/NetworkCard(点号固定输入,与设备端同交互)', () => {
  const info = {
    ifname: 'eth0',
    ip: '192.168.137.100',
    netmask: '255.255.255.0',
    gateway: '192.168.137.1',
    have_ip: true,
    online: true,
    mode: 'static',
    configured: {
      mode: 'static',
      ip: '192.168.137.100',
      netmask: '255.255.255.0',
      gateway: '192.168.137.1',
    },
  }
  const mountCard = () => mount(NetworkCard, { props: { info } })
  const addrInputs = (w) => w.findAll('.fields input')

  it('回填走补零形态(规范值 255.255.255.0 → 255.255.255.000)', async () => {
    const w = mountCard()
    await w.find('input[value="static"]').setValue()
    const [ip, mask, gw] = addrInputs(w)
    expect(ip.element.value).toBe('192.168.137.100')   // 全段 3 位,恒等
    expect(mask.element.value).toBe('255.255.255.000')
    expect(gw.element.value).toBe('192.168.137.001')
  })

  it('输入只敲数字:每 3 位自动插点号,13 位截到 12', async () => {
    const w = mountCard()
    await w.find('input[value="static"]').setValue()
    const ip = w.findAll('.fields input')[0]
    await ip.setValue('192168001050')
    expect(ip.element.value).toBe('192.168.001.050')
    await ip.setValue('1234567890123')
    expect(ip.element.value).toBe('123.456.789.012')   // 展示层不判段值
  })

  it('不满 12 位:红字提示且应用不可用;输满恢复', async () => {
    const w = mountCard()
    await w.find('input[value="static"]').setValue()
    const [ip, mask] = addrInputs(w)
    await ip.setValue('192168')
    await ip.trigger('blur')
    expect(w.text()).toContain('请输满 12 位数字,点号自动补全')
    const disabled = () =>
      w.findAll('button').find((b) => b.text().includes('应用配置'))
    expect(disabled().attributes('disabled')).toBeDefined()
    await ip.setValue('192168001050')
    expect(w.text()).not.toContain('请输满 12 位数字')
  })

  it('应用载荷规范化:补零形态提交为规范形', async () => {
    const w = mountCard()
    await w.find('input[value="static"]').setValue()
    const [ip, mask, gw] = addrInputs(w)
    await ip.setValue('192168001050')
    await mask.setValue('255255255000')
    await gw.setValue('192168137001')
    const apply = w
      .findAll('button')
      .find((b) => b.text().includes('应用配置'))
    await apply.trigger('click')
    const payload = w.emitted('apply')[0][0]
    expect(payload.mode).toBe('static')
    expect(payload.ip).toBe('192.168.1.50')
    expect(payload.netmask).toBe('255.255.255.0')
    expect(payload.gateway).toBe('192.168.137.1')
  })

  it('网关清空 = 不设网关(空串载荷)', async () => {
    const w = mountCard()
    await w.find('input[value="static"]').setValue()
    const [ip, mask, gw] = addrInputs(w)
    await gw.setValue('')
    await gw.trigger('blur')
    const apply = w
      .findAll('button')
      .find((b) => b.text().includes('应用配置'))
    await apply.trigger('click')
    expect(w.emitted('apply')[0][0].gateway).toBe('')
  })
})

  it('轮询回填不覆盖用户输入(动过表单后 info 刷新不再回填)', async () => {
    const info = {
      ifname: 'eth0',
      ip: '192.168.137.100',
      netmask: '255.255.255.0',
      gateway: '192.168.137.1',
      have_ip: true,
      online: true,
      mode: 'static',
      configured: {
        mode: 'static',
        ip: '192.168.137.100',
        netmask: '255.255.255.0',
        gateway: '192.168.137.1',
      },
    }
    const w = mount(NetworkCard, { props: { info } })
    await w.find('input[value="static"]').setValue()
    const ip = w.findAll('.fields input')[0]
    await ip.setValue('192168001050')
    await w.setProps({ info: { ...info } })   // 模拟轮询刷新(新对象触发 watch)
    expect(ip.element.value).toBe('192.168.001.050')  // 用户输入保持
  })
