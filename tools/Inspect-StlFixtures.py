#!/usr/bin/env python3
"""Inventory reference STL fixtures; format/topology diagnostics are not solid validation."""
import argparse, hashlib, json, math, struct
from pathlib import Path

class AsciiStlError(ValueError):
    pass

def _numbers(tokens, line_number, record):
    if len(tokens) != 3:
        raise AsciiStlError(f"line {line_number}: {record} requires exactly three numeric values")
    try:
        values = tuple(float(token) for token in tokens)
    except ValueError:
        raise AsciiStlError(f"line {line_number}: {record} has an invalid numeric value") from None
    if not all(math.isfinite(value) for value in values):
        raise AsciiStlError(f"line {line_number}: {record} has a non-finite numeric value")
    return values

def _ascii_stl_vertices(text):
    lines = [(number, line.strip()) for number, line in enumerate(text.splitlines(), 1) if line.strip()]
    vertices = []
    index = 0
    solids = 0
    while index < len(lines):
        line_number, line = lines[index]
        if not line.lower().startswith("solid") or (len(line) > 5 and not line[5].isspace()):
            raise AsciiStlError(f"line {line_number}: expected solid")
        solids += 1
        index += 1
        while index < len(lines):
            line_number, line = lines[index]
            keyword = line.split(None, 1)[0].lower()
            if keyword == "endsolid":
                index += 1
                break
            if keyword != "facet":
                raise AsciiStlError(f"line {line_number}: expected facet or endsolid")
            tokens = line.split()
            if len(tokens) != 5 or tokens[1].lower() != "normal":
                raise AsciiStlError(f"line {line_number}: facet requires normal and three numeric values")
            _numbers(tokens[2:], line_number, "facet normal")
            index += 1
            if index >= len(lines):
                raise AsciiStlError(f"line {line_number}: missing outer loop")
            loop_number, loop = lines[index]
            if loop.lower().split() != ["outer", "loop"]:
                raise AsciiStlError(f"line {loop_number}: expected outer loop")
            index += 1
            face = []
            for _ in range(3):
                if index >= len(lines):
                    raise AsciiStlError(f"line {loop_number}: facet has fewer than three vertices")
                vertex_number, vertex = lines[index]
                tokens = vertex.split()
                if not tokens or tokens[0].lower() != "vertex":
                    raise AsciiStlError(f"line {vertex_number}: expected vertex")
                face.append(_numbers(tokens[1:], vertex_number, "vertex"))
                index += 1
            if index >= len(lines):
                raise AsciiStlError(f"line {loop_number}: missing endloop")
            endloop_number, endloop = lines[index]
            if endloop.lower().split() != ["endloop"]:
                raise AsciiStlError(f"line {endloop_number}: expected endloop after three vertices")
            index += 1
            if index >= len(lines):
                raise AsciiStlError(f"line {loop_number}: missing endfacet")
            endfacet_number, endfacet = lines[index]
            if endfacet.lower().split() != ["endfacet"]:
                raise AsciiStlError(f"line {endfacet_number}: expected endfacet")
            vertices.extend(face)
            index += 1
        else:
            raise AsciiStlError("missing endsolid")
    if not solids:
        raise AsciiStlError("missing solid")
    return vertices

def inspect(path, root):
    data = path.read_bytes(); text = None
    try: text = data.decode("ascii")
    except UnicodeDecodeError: pass
    binary_exact = len(data) >= 84 and (len(data)-84) % 50 == 0 and data[80:84] == struct.pack("<I", (len(data)-84)//50)
    fmt = "binary" if binary_exact else "unknown"
    verts=[]; triangles=0; malformed=False; diagnostic=None
    if fmt == "binary":
        triangles=struct.unpack_from("<I",data,80)[0]
        for i in range(triangles):
            off=84+i*50
            verts.extend(struct.unpack_from("<9f",data,off+12))
        verts=[tuple(verts[i:i+3]) for i in range(0,len(verts),3)]
        if not all(math.isfinite(v) for p in verts for v in p):
            fmt="unknown"; verts=[]; triangles=0; malformed=True
            diagnostic="binary STL contains non-finite vertex coordinates"
    elif text is not None:
        try:
            verts = _ascii_stl_vertices(text)
            triangles = len(verts) // 3
            fmt = "ascii"
        except AsciiStlError as error:
            malformed=True; diagnostic=str(error)
    finite=(not malformed) and all(math.isfinite(v) for p in verts for v in p)
    bounds=None
    if verts and finite:
        bounds=[[min(p[i] for p in verts),max(p[i] for p in verts)] for i in range(3)]
    edges={}
    for i in range(triangles):
        tri=verts[i*3:i*3+3]
        if len(tri)<3: continue
        for a,b in ((tri[0],tri[1]),(tri[1],tri[2]),(tri[2],tri[0])):
            e=tuple(sorted((a,b))); edges[e]=edges.get(e,0)+1
    counts={};
    for n in edges.values(): counts[str(n)]=counts.get(str(n),0)+1
    result={"path":path.relative_to(root).as_posix(),"role":"container" if path.parent.name=="containers" else "item",
      "units":"mm (user-confirmed)","interior_volume_semantics":"whole closed container volume; user-confirmed (geometry validity unverified)" if path.parent.name=="containers" else None,
      "format":fmt,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"triangles":triangles,
      "finite_coordinates":finite,"bounds_source_units":bounds,"edge_multiplicity":counts,
      "boundary_edges":counts.get("1",0),"nonmanifold_edges":sum(v for k,v in counts.items() if int(k)>2),
      "topology_indicators_are_not_solid_validation":True}
    if diagnostic is not None:
        result["parse_diagnostic"] = diagnostic
    return result

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--root",type=Path,default=Path(".")); ap.add_argument("--out",type=Path,default=Path("tests/fixtures/rc-manifest.json")); ap.add_argument("--diagnostics",type=Path,default=Path(".local/fixture-inspection")); a=ap.parse_args()
    root=a.root.resolve(); files=sorted(p for d in (root/"rc"/"items",root/"rc"/"containers") for p in d.glob("*") if p.is_file() and p.suffix.lower()==".stl")
    records=[inspect(p,root) for p in files]; a.out.parent.mkdir(parents=True,exist_ok=True); a.diagnostics.mkdir(parents=True,exist_ok=True)
    doc={"schema_version":1,"source":"rc reference models","units":"mm (user-confirmed)","permission":"user-supplied for local tests; distribution permission unknown","records":records}
    a.out.write_text(json.dumps(doc,indent=2)+"\n",encoding="utf-8"); (a.diagnostics/"inventory.json").write_text(json.dumps(doc,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"files":len(records),"triangles":sum(r["triangles"] for r in records),"manifest":str(a.out)},indent=2))
if __name__ == "__main__": main()
