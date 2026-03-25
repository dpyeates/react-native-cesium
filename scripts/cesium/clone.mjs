#!/usr/bin/env node
/**
 * Clone or update CesiumGS/cesium-native into vendor/cesium-native (gitignored).
 */
import { execFileSync } from 'node:child_process'
import { existsSync, mkdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..', '..')
const vendor = join(root, 'vendor')
const dest = join(vendor, 'cesium-native')
const repo = 'https://github.com/CesiumGS/cesium-native.git'

function run(cmd, args, opts = {}) {
  execFileSync(cmd, args, { stdio: 'inherit', ...opts })
}

mkdirSync(vendor, { recursive: true })

if (!existsSync(dest)) {
  run('git', ['clone', '--depth', '1', repo, dest], { cwd: root })
  console.log(`Cloned Cesium Native into ${dest}`)
} else {
  run('git', ['-C', dest, 'fetch', 'origin'])
  run('git', ['-C', dest, 'pull', '--ff-only'])
  console.log(`Updated Cesium Native at ${dest}`)
}
