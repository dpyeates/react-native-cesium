#!/usr/bin/env node
/**
 * Clone or update CesiumGS/cesium-native at a pinned tag into vendor/cesium-native (gitignored).
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
const ref = 'v0.59.0'

function run(cmd, args, opts = {}) {
  execFileSync(cmd, args, { stdio: 'inherit', ...opts })
}

mkdirSync(vendor, { recursive: true })

if (!existsSync(dest)) {
  run('git', ['clone', '--branch', ref, '--depth', '1', repo, dest], {
    cwd: root,
  })
  console.log(`Cloned Cesium Native ${ref} into ${dest}`)
} else {
  run('git', ['-C', dest, 'fetch', '--depth', '1', 'origin', 'tag', ref])
  run('git', ['-C', dest, 'checkout', '--detach', '--force', ref])
  console.log(`Updated Cesium Native at ${dest} to ${ref}`)
}
