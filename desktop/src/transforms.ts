import { BufferGeometry, InstancedMesh, Matrix4, MeshStandardMaterial, Quaternion, Vector3 } from 'three';
import type { Results } from './generated/contracts';

export function poseMatrix(pose: Results.Placement): Matrix4 {
  return new Matrix4().compose(new Vector3(...pose.translation_mm), new Quaternion(...pose.quaternion_xyzw), new Vector3(1, 1, 1));
}
export function packingMesh(geometry: BufferGeometry, poses: Results.Placement[], hidden: ReadonlySet<string>): { mesh: InstancedMesh; ids: string[] } {
  const visible = poses.filter(p => !hidden.has(p.copy_id));
  const mesh = new InstancedMesh(geometry, new MeshStandardMaterial({ color: '#52caba', roughness: .42, metalness: .12 }), Math.max(visible.length, 1));
  mesh.count = visible.length;
  visible.forEach((pose, index) => mesh.setMatrixAt(index, poseMatrix(pose)));
  mesh.instanceMatrix.needsUpdate = true;
  return { mesh, ids: visible.map(p => p.copy_id) };
}
