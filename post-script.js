/**
 * @file Post-codegen workaround for Android Nitro bindings.
 *
 * @description Strips the `margelo/nitro/` prefix from includes inside the
 * generated `ReactNativeCesiumOnLoad.cpp` file so that the module resolves
 * correctly under a custom package name. Run automatically as part of the
 * `codegen` npm script (after `nitrogen`).
 */
const path = require('node:path')
const { writeFile, readFile } = require('node:fs/promises')

async function androidWorkaround() {
  const androidOnLoadFile = path.join(
    process.cwd(),
    'nitrogen/generated/android',
    'ReactNativeCesiumOnLoad.cpp',
  )
  const str = await readFile(androidOnLoadFile, { encoding: 'utf8' })
  await writeFile(androidOnLoadFile, str.replace(/margelo\/nitro\//g, ''))
}

androidWorkaround().catch((err) => {
  console.error('[post-script] Android workaround failed:', err)
  process.exit(1)
})
