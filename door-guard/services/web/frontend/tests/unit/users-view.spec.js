/*
 * users-view.spec.js — 用户管理页:IC 卡 UI 挂起门控(IC_UI_ENABLED=false)
 *
 * 只锁"UI 不出现"这一件事:list 无 IC 列、验证方式勾选与列无 IC 字样、
 * 编辑态无绑定/解绑块;listUsers 返回带 has_ic/ic_mask 与 auth_flags 含
 * bit8 的历史数据,确认老数据不丢、只是不展示。API 层不在本页范围。
 */
import { describe, expect, it, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'

const listUsers = vi.fn()
vi.mock('../../src/api/users', async (importOriginal) => {
  const actual = await importOriginal()
  return { ...actual, listUsers: (...args) => listUsers(...args) }
})
vi.mock('../../src/stores/toast', () => ({
  toast: { ok: vi.fn(), err: vi.fn(), info: vi.fn() },
}))
vi.mock('../../src/stores/events', () => ({ onEnrollResult: () => () => {} }))

import UsersView from '../../src/views/UsersView.vue'

/* 历史用户:auth_flags 带密码+IC(bit4|bit8),IC 已绑定 */
const ROW = {
  uid: '10001', name: '张三', role: 0,
  auth_flags: 4 | 8,
  has_face: true, has_finger: false, has_ic: true, ic_mask: '04A3B2C1',
}

async function mountView() {
  listUsers.mockResolvedValue({
    users: [ROW], page: 1, pages: 1, total: 1,
  })
  const wrapper = mount(UsersView)
  await flushPromises()
  return wrapper
}

describe('views/UsersView — IC UI 挂起', () => {
  it('列表无 IC 卡列,验证方式列不出现 IC 字样', async () => {
    const wrapper = await mountView()
    const headers = wrapper.findAll('th').map((th) => th.text())
    expect(headers).not.toContain('IC 卡')
    expect(headers).toContain('人脸')
    const body = wrapper.text()
    expect(body).toContain('密码')
    expect(body).toContain('张三')
    expect(body).not.toContain('IC 卡')
  })

  it('顶部说明文案不提 IC 卡', async () => {
    const wrapper = await mountView()
    expect(wrapper.text()).toContain('指纹在设备端录入')
    expect(wrapper.text()).not.toContain('刷卡')
  })

  it('编辑态:无绑定/解绑块,验证方式勾选只有人脸/密码/指纹', async () => {
    const wrapper = await mountView()
    const editBtn = wrapper.findAll('button').find((b) => b.text() === '编辑')
    expect(editBtn).toBeTruthy()
    await editBtn.trigger('click')
    await flushPromises()

    expect(wrapper.find('.ic-bind').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('录入 IC 卡')
    expect(wrapper.text()).not.toContain('解绑')

    const flags = wrapper.findAll('.flags__item').map((el) => el.text())
    expect(flags).toHaveLength(3)
    expect(flags.join('/')).not.toContain('IC 卡')
    expect(flags.join('/')).toContain('人脸')
  })
})
