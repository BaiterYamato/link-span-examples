"""Package the Policroma DX11 sample for the shaderpacks directory."""

import argparse
import json
from pathlib import Path
import zipfile


def build(output: Path, fog_sun=False, point_shadow=False, point_lights=1):
    source = Path(__file__).resolve().parent / "packs" / "KokiriUnbound"
    files = {
        file.relative_to(source).as_posix(): file.read_bytes()
        for file in (source / "shaders").rglob("*")
        if file.is_file()
    }
    manifest_path = "shaders/linkspan-v2.json"
    manifest = json.loads(files[manifest_path])
    manifest["transmissionMaterials"] = json.loads(
        (source / "transmission-materials.json").read_bytes()
    )
    manifest["defaultProfile"] = "vidro-cena" if point_shadow else "equilibrado"
    manifest["profiles"] = [
        {"id": profile, "label": label, "defines": {"POLICROMA_AO": mode}}
        for profile, label, mode in (
            ("equilibrado", "Oclusão equilibrada", 1),
            ("sem-ao", "Sem oclusão", 0),
            ("forte", "Oclusão forte", 2),
            ("diagnostico", "Máscara de oclusão", 3),
        )
    ]
    manifest["profiles"].append({
        "id": "vidro-cena",
        "label": "Vidro com luz da cena",
        "defines": {"POLICROMA_AO": 1, "POLICROMA_GLASS_LIGHTING": 1},
    })
    for shader_pass in manifest["passes"]:
        shader_pass["inputs"].append("scene.fogshadow" if fog_sun else "scene.geometryshadow")
        name = shader_pass["file"]
        defines = b"#define KOKIRI_STATIC_SUN_SHADOW 1\n"
        if shader_pass["name"] == "world":
            defines += (
                b"#define KOKIRI_SHADOWED_WATER 1\n"
                b"#define KOKIRI_DEMO_GLASS 1\n"
                b"#define KOKIRI_MIXED_GLASS 1\n"
                b"#define KOKIRI_TRANSMISSION_CONE float3(-0.64, -0.768375, 0.0)\n"
            )
            if not point_shadow:
                defines += b"#define KOKIRI_MATERIAL_TRANSMISSION 1\n"
        files[name] = defines + files[name]
    glass = "shaders/glass.hlsl"
    if point_shadow:
        manifest["passes"].append({
            "name": "glass", "file": glass, "divisor": 1,
            "inputs": ["world", "scene.depth", "scene.pointshadow"],
        })
        files[glass] = (
            "#define KOKIRI_POINT_SHADOW 1\n"
            f"#define KOKIRI_POINT_SHADOW_LIGHTS {point_lights}\n"
            "#define KOKIRI_TRANSMISSION_CONE float3(-0.64, -0.768375, 0.0)\n"
        ).encode("ascii") + files[glass]
    else:
        del files[glass]
    files[manifest_path] = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")
    write_zip(output, files)


def write_zip(output: Path, files):
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in sorted(files.items()):
            archive.writestr(zipfile.ZipInfo(name), data, compress_type=zipfile.ZIP_DEFLATED)


def build_glsl(output: Path):
    source = Path(__file__).resolve().parent / "packs" / "PolicromaGlsl"
    write_zip(output, {file.relative_to(source).as_posix(): file.read_bytes()
                       for file in (source / "shaders").rglob("*") if file.is_file()})


def build_iris(output: Path):
    # Keeps the .vsh files: the catalog classifies a ZIP as Iris by its .vsh/.fsh pairs.
    source = Path(__file__).resolve().parent / "packs" / "IrisMinimo"
    write_zip(output, {file.relative_to(source).as_posix(): file.read_bytes()
                       for file in (source / "shaders").rglob("*") if file.is_file()})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Caminho de saída do ZIP")
    parser.add_argument("--glsl", action="store_true", help="Empacotar a demonstração GLSL")
    parser.add_argument("--iris", action="store_true", help="Empacotar a demonstração Iris reduzida")
    parser.add_argument("--fog-sun", action="store_true", help="Usar o mapa de sombras na iluminação da névoa")
    parser.add_argument("--point-shadow", action="store_true", help="Rasterizar sombras da luz que atravessa o vidro")
    parser.add_argument("--point-lights", type=int, choices=(1, 2), default=1, help="Quantidade de fontes com mapa de sombra")
    options = parser.parse_args()
    if options.iris:
        build_iris(options.output)
    elif options.glsl:
        build_glsl(options.output)
    else:
        build(options.output, options.fog_sun, options.point_shadow, options.point_lights)
