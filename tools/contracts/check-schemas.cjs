const fs = require('fs');
const path = require('path');
const Ajv = require('ajv');
const addFormats = require('ajv-formats');

const root = path.resolve(__dirname, '..', '..');
const schemas = fs.readdirSync(path.join(root, 'spec/schemas')).filter(x => x.endsWith('.schema.json'))
  .map(x => JSON.parse(fs.readFileSync(path.join(root, 'spec/schemas', x), 'utf8')));
const ajv = new Ajv({strict: false, allErrors: true}); addFormats(ajv);
schemas.forEach(schema => ajv.addSchema(schema));
const fixtures = JSON.parse(fs.readFileSync(path.join(root, 'tests/contracts/schema-fixtures.json'), 'utf8'));
let failed = 0;
const names = new Set();
for (const item of fixtures) {
  if (!item || typeof item.name !== 'string' || names.has(item.name) || !['settings','assets','results','protocol','benchmark-summary'].includes(item.kind) || typeof item.schema_valid !== 'boolean' || typeof item.semantic_valid !== 'boolean' || !Object.prototype.hasOwnProperty.call(item, 'value')) {
    console.error(`invalid fixture record: ${item && item.name}`); failed++; continue;
  }
  names.add(item.name);
  const validate = ajv.getSchema(`https://spectrapack.invalid/schemas/v1/${item.kind}.schema.json`);
  const actual = validate(item.value);
  if (actual !== item.schema_valid) { console.error(`${item.name}: expected schema_valid=${item.schema_valid}, got ${actual}: ${ajv.errorsText(validate.errors)}`); failed++; }
  if (!item.schema_valid && item.semantic_valid) { console.error(`${item.name}: semantic_valid cannot be true when schema_valid is false`); failed++; }
}
if (failed) process.exit(1);
const counts = Object.fromEntries(['settings','assets','results','protocol','benchmark-summary'].map(k => [k, fixtures.filter(x => x.kind === k).length]));
console.log(`${fixtures.length} fixture records passed (${JSON.stringify(counts)})`);
