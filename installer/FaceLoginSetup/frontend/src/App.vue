<script lang="ts" setup>
import { ref, onMounted } from 'vue'
import { GetDefaultPaths, Install, Uninstall, PickDirectory, IsInstalled, LaunchConsole } from '../wailsjs/go/main/App'
import { EventsOn, EventsOff, Quit } from '../wailsjs/runtime'

const installDir = ref('')
const showInstall = ref(true)
const running = ref(false)
const progressPercent = ref(0)
const progressStep = ref('')
const progressStatus = ref('')
const progressDetail = ref('')
const resultMessage = ref('')
const resultSuccess = ref(false)
const showResult = ref(false)
const alreadyInstalled = ref(false)
const installDone = ref(false)
const launchChecked = ref(true)
const finishError = ref('')
const finishing = ref(false)

onMounted(async () => {
  try {
    const paths = await GetDefaultPaths()
    installDir.value = paths.installDir
    alreadyInstalled.value = await IsInstalled()
  } catch (err: any) {
    // Keep installDir non-empty so the install button isn't permanently disabled
    // on a marshalling failure — surface the error as the result banner instead.
    installDir.value = 'C:\\Program Files\\FaceLogin'
    resultMessage.value = '初始化失败：' + (err?.message || String(err))
    resultSuccess.value = false
    showResult.value = true
  }
})

async function doPickDirectory() {
  try {
    const dir = await PickDirectory()
    if (dir) {
      installDir.value = dir
    }
  } catch (err: any) {
    resultMessage.value = '选择目录失败：' + (err?.message || String(err))
    resultSuccess.value = false
    showResult.value = true
  }
}

function toggleMode(mode: string) {
  showInstall.value = mode === 'install'
  showResult.value = false
  installDone.value = false
  finishError.value = ''
  progressStatus.value = ''
}

async function doInstall() {
  running.value = true
  showResult.value = false
  installDone.value = false
  finishError.value = ''
  progressPercent.value = 0
  progressStatus.value = 'running'

  // Clear any listener left over from a previous run that never reached 100%
  // (e.g. a backend failure path returns before emitting the final event),
  // then register a fresh one. Always torn down in the finally below.
  EventsOff('setup:progress')
  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = e.detail || ''
  })

  try {
    const result = await Install(installDir.value)
    resultMessage.value = result.message
    resultSuccess.value = result.success
    // Success goes to the dedicated finish page (checkbox + finish button);
    // failure keeps the old result banner with a back button.
    if (result.success) {
      installDone.value = true
    } else {
      showResult.value = true
    }

  } catch (err: any) {
    resultMessage.value = err?.message || String(err) || '安装失败'
    resultSuccess.value = false
    showResult.value = true
  } finally {
    running.value = false
    EventsOff('setup:progress')
  }
}

async function doFinish() {
  finishing.value = true
  finishError.value = ''
  try {
    if (launchChecked.value) {
      const r = await LaunchConsole()
      if (!r.success) {
        finishError.value = r.message || '打开注册向导失败'
        finishing.value = false
        return
      }
    }
    await Quit()
  } catch (err: any) {
    finishError.value = err?.message || String(err)
    finishing.value = false
  }
}

async function doUninstall() {
  if (!confirm('确定要卸载 FaceLogin 人脸登录吗？\n\n⚠️ 卸载将删除所有程序文件、人脸数据和日志，且不可恢复！')) return

  running.value = true
  showResult.value = false
  progressPercent.value = 0
  progressStatus.value = 'running'

  EventsOff('setup:progress')
  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = e.detail || ''
  })

  try {
    const result = await Uninstall()
    resultMessage.value = result.message
    resultSuccess.value = result.success
    showResult.value = true
  } catch (err: any) {
    resultMessage.value = err?.message || String(err) || '卸载失败'
    resultSuccess.value = false
    showResult.value = true
  } finally {
    running.value = false
    EventsOff('setup:progress')
  }
}
</script>

<template>
  <div class="flex flex-col h-screen bg-white select-none">
    <!-- Header -->
    <div class="px-8 pt-8 pb-2">
      <h1 class="text-2xl font-light tracking-tight text-gray-900">FaceLogin</h1>
      <p class="text-sm text-gray-400 font-light">人脸识别登录系统 · 安装程序</p>
    </div>

    <!-- Mode Tabs -->
    <div class="px-8 mt-4 flex gap-6 border-b border-gray-100">
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('install')"
        :disabled="running"
      >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 !showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('uninstall')"
        :disabled="running"
      >卸载</button>
    </div>

    <!-- Body -->
    <div class="flex-1 px-8 py-6">
      <!-- Install mode -->
      <div v-if="showInstall && !running && !showResult && !installDone">
        <label class="block text-xs text-gray-500 uppercase tracking-wider mb-2">安装目录</label>
        <div class="flex items-center gap-3">
          <input
            v-model="installDir"
            class="flex-1 px-3 py-2 text-sm border border-gray-200 bg-gray-50
                   focus:outline-none focus:border-gray-400 transition-colors text-gray-800"
            placeholder="选择安装目录"
          />
          <button
            class="flex-shrink-0 w-9 h-9 flex items-center justify-center border border-gray-200
                   hover:bg-gray-100 transition-colors text-gray-500"
            @click="doPickDirectory"
            title="选择文件夹"
          >
            <svg xmlns="http://www.w3.org/2000/svg" class="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
            </svg>
          </button>
        </div>
        <p class="mt-1 text-xs text-gray-400">将安装到该目录：程序文件 + 模型文件（约 50 MB：检测 + 识别 + 双活体）放入 models/ 子目录</p>
        <p class="mt-1 text-xs text-gray-400">建议保留默认目录：FaceLogin 运行于登录认证链路，<code class="text-gray-500">C:\Program Files\</code> 受系统级 ACL 保护，可防止程序文件被篡改。</p>

        <button
          class="mt-6 w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doInstall"
          :disabled="!installDir"
        >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      </div>

      <!-- Uninstall mode -->
      <div v-if="!showInstall && !running && !showResult">
        <p class="text-sm text-gray-600 leading-relaxed">
          卸载将停止并删除 FaceLogin 服务、注销登录组件、删除全部程序文件，
          以及 <strong>人脸数据和日志（不可恢复）</strong>，并移除安装目录。
        </p>
        <button
          class="mt-6 w-full py-2.5 text-sm font-medium border border-gray-300 text-gray-700
                 hover:bg-gray-100 transition-colors disabled:opacity-40"
          @click="doUninstall"
        >卸载 FaceLogin</button>
      </div>

      <!-- Progress -->
      <div v-if="running" class="space-y-4">
        <div class="relative h-0.5 bg-gray-100">
          <div
            class="absolute top-0 left-0 h-full bg-gray-900 transition-all duration-300 ease-out"
            :style="{ width: progressPercent + '%' }"
          ></div>
        </div>
        <div class="flex items-center justify-between">
          <span class="text-sm text-gray-800">{{ progressStep }}</span>
          <span class="text-xs text-gray-400">{{ progressPercent }}%</span>
        </div>
        <p v-if="progressDetail" class="text-xs text-gray-400">{{ progressDetail }}</p>
      </div>

      <!-- Result -->
      <div v-if="showResult && !running" class="space-y-4">
        <div
          :class="['text-sm whitespace-pre-line leading-relaxed',
                   resultSuccess ? 'text-gray-800' : 'text-red-600']"
        >
          {{ resultSuccess ? '✓ ' : '✗ ' }}{{ resultMessage }}
        </div>
        <button
          class="w-full py-2 text-sm text-gray-500 border border-gray-200
                 hover:bg-gray-50 transition-colors"
          @click="showResult = false"
        >返回</button>
      </div>

      <!-- Install finish page: success + optional console launch -->
      <div v-if="installDone && !running" class="space-y-6">
        <div class="flex items-center gap-4">
          <div class="w-10 h-10 rounded-full bg-green-50 flex items-center justify-center flex-shrink-0">
            <svg xmlns="http://www.w3.org/2000/svg" class="w-5 h-5 text-green-600" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <path d="M20 6 9 17l-5-5"/>
            </svg>
          </div>
          <div>
            <p class="text-lg font-medium text-gray-900">安装成功</p>
            <p class="text-sm text-gray-500 leading-relaxed">人脸认证服务已启动，登录界面已启用人脸选项。<br>注册人脸后即可使用人脸登录。</p>
          </div>
        </div>
        <label class="flex items-center gap-2.5 text-sm text-gray-700 cursor-pointer select-text">
          <input type="checkbox" v-model="launchChecked" class="w-4 h-4 accent-gray-900" />
          立即打开人脸注册向导
        </label>
        <p v-if="finishError" class="text-xs text-red-600">{{ finishError }}</p>
        <button
          class="w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doFinish"
          :disabled="finishing"
        >完成</button>
      </div>
    </div>

    <!-- Footer -->
    <div class="px-8 py-4 border-t border-gray-100">
      <p class="text-xs text-gray-300">Windows 人脸识别登录</p>
    </div>
  </div>

</template>
