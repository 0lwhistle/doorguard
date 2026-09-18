/*
 * components.spec.js — 组件行为(纯展示组件,用 props 驱动,不碰网络)
 */
import { describe, expect, it, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import AppField from '../../src/components/AppField.vue'
import DataTable from '../../src/components/DataTable.vue'
import EventFeed from '../../src/components/EventFeed.vue'
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
