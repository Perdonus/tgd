<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

const progress = ref(0)
const dragging = ref(false)
const track = ref<HTMLElement | null>(null)

const sync = () => {
  const max = document.documentElement.scrollHeight - window.innerHeight
  progress.value = max > 0 ? window.scrollY / max : 0
}

const setFromClientX = (clientX: number) => {
  if (!track.value) {
    return
  }
  const rect = track.value.getBoundingClientRect()
  const ratio = Math.min(1, Math.max(0, (clientX - rect.left) / rect.width))
  const max = document.documentElement.scrollHeight - window.innerHeight
  window.scrollTo({
    top: ratio * Math.max(0, max),
    behavior: dragging.value ? 'auto' : 'smooth',
  })
}

const onPointerMove = (event: PointerEvent) => {
  if (!dragging.value) {
    return
  }
  setFromClientX(event.clientX)
}

const onPointerUp = () => {
  dragging.value = false
}

const startDrag = (event: PointerEvent) => {
  dragging.value = true
  setFromClientX(event.clientX)
}

onMounted(() => {
  sync()
  window.addEventListener('scroll', sync, { passive: true })
  window.addEventListener('resize', sync)
  window.addEventListener('pointermove', onPointerMove)
  window.addEventListener('pointerup', onPointerUp)
})

onBeforeUnmount(() => {
  window.removeEventListener('scroll', sync)
  window.removeEventListener('resize', sync)
  window.removeEventListener('pointermove', onPointerMove)
  window.removeEventListener('pointerup', onPointerUp)
})

const thumbStyle = computed(() => ({
  left: `calc(${progress.value * 100}% - 22px)`,
}))
</script>

<template>
  <div class="docs-scroll-slider" aria-hidden="true">
    <div ref="track" class="docs-scroll-slider__track" @pointerdown="startDrag">
      <div class="docs-scroll-slider__fill" :style="{ width: `${progress * 100}%` }"></div>
      <div class="docs-scroll-slider__thumb" :style="thumbStyle"></div>
    </div>
  </div>
</template>
