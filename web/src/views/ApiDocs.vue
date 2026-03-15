<template>
  <div>
    <p style="color:#666;font-size:13px;margin:0 0 20px">
      在此页面可查看所有 API 接口并直接进行测试。
      所有接口受 Basic Auth 保护，此页面使用当前登录态发请求。
    </p>

    <div v-for="ep in endpoints" :key="ep.path" class="endpoint">
      <div class="endpoint-header" @click="ep._open = !ep._open">
        <span class="method-badge" :class="ep.method.toLowerCase()">{{ ep.method }}</span>
        <span class="endpoint-path">{{ ep.path }}</span>
        <span class="endpoint-summary">{{ ep.summary }}</span>
      </div>
      <div class="endpoint-body" :class="{ open: ep._open }">
        <p v-if="ep.desc" style="color:#666;font-size:13px;margin:0 0 16px">{{ ep.desc }}</p>

        <div v-if="ep.params?.length">
          <h4 style="margin:0 0 10px;font-size:14px">参数（application/x-www-form-urlencoded）</h4>
          <div v-for="p in ep.params" :key="p.name" class="form-group">
            <label>
              {{ p.name }}
              <span v-if="p.required" style="color:#e74c3c;font-size:12px"> *必填</span>
              <span style="color:#888;font-size:12px"> — {{ p.desc }}</span>
            </label>
            <input type="text" v-model="p._val" :placeholder="p.placeholder || ''">
          </div>
        </div>

        <button class="btn btn-blue btn-sm" :disabled="ep._loading" @click="runTest(ep)">
          {{ ep._loading ? '⏳ 请求中...' : '▶ 发起测试' }}
        </button>

        <div v-if="ep._response" class="response-area visible" style="margin-top:14px">
          <div class="status-badge" :class="ep._response.ok ? 'ok' : 'error'">
            HTTP {{ ep._response.status }}
          </div>
          <br>{{ ep._response.body }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import {reactive} from 'vue'

function ep(method, path, summary, params = [], desc = '') {
  return reactive({method, path, summary, params, desc, _open: false, _loading: false, _response: null})
}

function p(name, desc, placeholder = '', required = false) {
  return reactive({name, desc, placeholder, required, _val: ''})
}

const endpoints = [
  ep('GET', '/api/status', '获取设备状态（IP、SIM状态等）', [], '无需参数'),
  ep('GET', '/api/config', '获取当前配置（JSON）', [], '无需参数'),
  ep('POST', '/api/sendsms', '发送短信', [
    p('phone', '目标手机号', '13800138000', true),
    p('content', '短信内容', '测试内容...', true)
  ]),
  ep('GET', '/api/query', '模组信息查询', [
    p('type', '查询类型：ati / signal / siminfo / network / wifi', 'wifi', true)
  ]),
  ep('GET', '/api/flight', '飞行模式控制', [
    p('action', '操作：query / toggle / on / off', 'query', true)
  ]),
  ep('GET', '/api/at', '发送 AT 指令', [
    p('cmd', 'AT 指令', 'AT+CSQ', true)
  ]),
  ep('POST', '/api/ping', '向 8.8.8.8 发起 Ping（消耗少量流量）', [], '无需参数'),
  ep('POST', '/api/test_push', '测试推送通道', [
    p('type', '推送类型（1-12）', '1'),
    p('url', 'Webhook / 推送地址', 'https://...'),
    p('key1', '参数1（Token / Secret等）', ''),
    p('key2', '参数2', ''),
    p('body', '自定义模板体', '')
  ]),
]

async function runTest(ep) {
  ep._loading = true
  ep._response = null
  try {
    let url = ep.path
    let opts = {method: ep.method}
    const kvs = ep.params.filter(p => p._val)
    if (ep.method === 'GET' && kvs.length) {
      url += '?' + kvs.map(p => `${p.name}=${encodeURIComponent(p._val)}`).join('&')
    } else if (ep.method === 'POST' && kvs.length) {
      opts.headers = {'Content-Type': 'application/x-www-form-urlencoded'}
      opts.body = kvs.map(p => `${p.name}=${encodeURIComponent(p._val)}`).join('&')
    }
    const res = await fetch(url, opts)
    const ct = res.headers.get('content-type') || ''
    const body = ct.includes('json')
        ? JSON.stringify(await res.json(), null, 2)
        : await res.text()
    ep._response = {ok: res.ok, status: res.status, body}
  } catch (e) {
    ep._response = {ok: false, status: 0, body: e.message}
  } finally {
    ep._loading = false
  }
}
</script>

