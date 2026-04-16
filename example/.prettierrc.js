// Intentionally differs from the root library config:
// - trailingComma: 'all'  (vs 'es5' in the library) — example app targets a
//   single Node/Metro version so function-arg trailing commas are always valid.
module.exports = {
  arrowParens: 'avoid',
  singleQuote: true,
  trailingComma: 'all',
};
