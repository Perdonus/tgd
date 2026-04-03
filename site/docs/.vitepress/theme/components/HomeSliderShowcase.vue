<script setup lang="ts">
import { computed, ref } from 'vue'

const slider = ref(76)

const shellStyle = computed(() => ({
	width: `${54 + Math.round(slider.value * 0.32)}%`,
}))

const runtimeReadiness = computed(() => `${Math.min(99, slider.value + 18)}%`)
const uiCoverage = computed(() => `${Math.max(48, Math.round(slider.value * 0.82))}%`)
</script>

<template>
	<section class="home-slider">
		<div class="home-slider__copy">
			<p class="home-slider__eyebrow">Desktop-native runtime</p>
			<h2>Custom slider, host-side panel, no fragile dialog ownership</h2>
			<p class="home-slider__text">
				Astrogram plugins register settings with the client and let the host draw the
				panel. The result stays stable across theme changes, runtime updates and safe
				mode recovery.
			</p>
			<div class="home-slider__checks">
				<span>C++</span>
				<span>Qt</span>
				<span>Host UI</span>
			</div>
		</div>

		<div class="home-slider__panel">
			<div class="home-slider__panel-top">
				<div class="home-slider__dots" aria-hidden="true">
					<span></span>
					<span></span>
					<span></span>
				</div>
				<span class="home-slider__panel-title">AstroTransparent</span>
				<span class="home-slider__panel-value">{{ slider }}%</span>
			</div>

			<div class="home-slider__preview">
				<div class="home-slider__preview-shell" :style="shellStyle">
					<div class="home-slider__preview-line"></div>
					<div class="home-slider__preview-line short"></div>
					<div class="home-slider__preview-card"></div>
				</div>
			</div>

			<label class="home-slider__track" for="home-slider-range">
				<span>Custom slider</span>
				<input
					id="home-slider-range"
					v-model="slider"
					type="range"
					min="20"
					max="92"
					step="1"
				>
			</label>

			<div class="home-slider__metrics">
				<div>
					<strong>{{ runtimeReadiness }}</strong>
					<span>runtime readiness</span>
				</div>
				<div>
					<strong>{{ uiCoverage }}</strong>
					<span>host UI coverage</span>
				</div>
				<div>
					<strong>.tgd</strong>
					<span>native package format</span>
				</div>
			</div>
		</div>
	</section>
</template>
