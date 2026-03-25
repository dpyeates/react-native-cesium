#!/usr/bin/env node
/**
 * Remove outputs from `yarn run build` (native prebuilts, staging, CMake build trees).
 * Does not remove vendor/cesium-native source (`yarn update`) or vendor/vcpkg.
 * For other ignored artifacts (e.g. `lib/`, Android `.cxx/`), run `git clean -dfX` yourself
 * and exclude paths you need to keep (see `.gitignore`).
 */
import { existsSync, rmSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..', '..')
const cesiumSrc = join(root, 'vendor', 'cesium-native')

const paths = [
  join(root, 'vendor', 'android'),
  join(root, 'vendor', 'ios'),
  join(root, 'vendor', '.cesium-staging'),
  join(cesiumSrc, 'build-android-arm64'),
  join(cesiumSrc, 'build-ios-device'),
  join(cesiumSrc, 'build-ios-simulator'),
]

let removed = 0
for (const p of paths) {
  if (!existsSync(p)) continue
  rmSync(p, { recursive: true, force: true })
  console.log(`Removed ${p}`)
  removed++
}

if (removed === 0) {
  console.log('No Cesium native build outputs to remove (already clean).')
}
