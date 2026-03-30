const path = require('path');
const { getDefaultConfig, mergeConfig } = require('@react-native/metro-config');
const pak = require('../package.json');

const projectRoot = __dirname;
const monorepoRoot = path.resolve(projectRoot, '..');

// Alias peer + runtime dependencies to the example's copies (prevents duplicates in monorepo)
const modules = Object.keys({
  ...pak.peerDependencies,
  ...pak.dependencies,
});

/**
 * Metro configuration
 * https://reactnative.dev/docs/metro
 *
 * @type {import('@react-native/metro-config').MetroConfig}
 */
module.exports = mergeConfig(getDefaultConfig(__dirname), {
  watchFolders: [monorepoRoot],
  resolver: {
    extraNodeModules: modules.reduce((acc, name) => {
      acc[name] = path.join(projectRoot, 'node_modules', name);
      return acc;
    }, {}),
  },
});
