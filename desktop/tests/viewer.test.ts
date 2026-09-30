import { describe, expect, it, vi } from 'vitest';
import { Box3, Matrix4, Vector3 } from 'three';
import { PLYLoader } from 'three/addons/loaders/PLYLoader.js';
import { WebGLAttributes } from 'three/src/renderers/webgl/WebGLAttributes.js';
import type { Results } from '../src/generated/contracts';
import { packingMesh } from '../src/transforms';
import nativePreview from './fixtures/cube10mm-double.json';

function nativeCube() {
  // Real desktop-prepare output for the analytic 10 mm cube: binary PLY,
  // double XYZ, 8 shared local-mm vertices, 12 outward triangles.
  const bytes = Uint8Array.from(atob(nativePreview.ply_base64), c => c.charCodeAt(0)).buffer;
  return { bytes, geometry: new PLYLoader().parse(bytes) };
}

function uploadAttributes(geometry: ReturnType<typeof nativeCube>['geometry']) {
  // Exercise Three's actual WebGL upload adapter. The device records uploads;
  // no mock supplies geometry, transforms, or supported-buffer decisions.
  const gl = { createBuffer: vi.fn(() => ({})), bindBuffer: vi.fn(), bufferData: vi.fn(), FLOAT: 5126, UNSIGNED_SHORT: 5123, UNSIGNED_INT: 5125, ARRAY_BUFFER: 34962, ELEMENT_ARRAY_BUFFER: 34963 };
  const attributes = new WebGLAttributes(gl as unknown as WebGL2RenderingContext);
  attributes.update(geometry.getAttribute('position'), gl.ARRAY_BUFFER);
  attributes.update(geometry.getAttribute('normal'), gl.ARRAY_BUFFER);
  attributes.update(geometry.index!, gl.ELEMENT_ARRAY_BUFFER);
  return attributes.get(geometry.getAttribute('position'));
}

describe('UI-02 / AT-15 native double PLY display upload', () => {
  it('uploads the native preview and all 64 rigid copies in the 40 mm box', () => {
    const { bytes, geometry } = nativeCube();
    const original = new Uint8Array(bytes).slice();
    geometry.computeVertexNormals();
    const poses: Results.Placement[] = [];
    for (const x of [5, 15, 25, 35]) for (const y of [5, 15, 25, 35]) for (const z of [5, 15, 25, 35]) {
      poses.push({ copy_id: `cube-${x}-${y}-${z}`, translation_mm: [x,y,z], quaternion_xyzw: [0,0,0,1], local_to_world: [[1,0,0,x],[0,1,0,y],[0,0,1,z],[0,0,0,1]] });
    }
    const before = JSON.stringify(poses);
    const { mesh, ids } = packingMesh(geometry, poses, new Set());
    expect(uploadAttributes(mesh.geometry)?.type).toBe(5126);
    expect(mesh.count).toBe(64); expect(ids).toHaveLength(64);
    const bounds = new Box3().setFromObject(mesh);
    expect(bounds.min.toArray()).toEqual([0,0,0]); expect(bounds.max.toArray()).toEqual([40,40,40]);
    const matrix = new Matrix4(); mesh.getMatrixAt(0, matrix);
    expect(new Vector3(-5,-5,-5).applyMatrix4(matrix).toArray()).toEqual([0,0,0]);
    expect(new Uint8Array(bytes)).toEqual(original); expect(JSON.stringify(poses)).toBe(before);
    mesh.dispose(); (mesh.material as { dispose: () => void }).dispose(); geometry.dispose();
  });
  it('rejects finite native coordinates that overflow the display representation', () => {
    const { bytes } = nativeCube();
    const headerEnd = new TextDecoder().decode(bytes).indexOf('end_header\n') + 'end_header\n'.length;
    new DataView(bytes).setFloat64(headerEnd, 1e40, true);
    const geometry = new PLYLoader().parse(bytes);
    expect(() => packingMesh(geometry, [], new Set())).toThrow(/display.*range/i);
    geometry.dispose();
  });
});
