"""SM5 DXBC 着色器：从 UE4 材质包里提取、解析 RDEF/签名，并做最小反汇编。

游戏把每个着色器以 zlib 流的形式塞在 .uexp 里，解压后是
「一段其它头 + DXBC 容器 + 着色器参数表」，因此这里先在解压数据里定位
DXBC 魔数，再从魔数处按标准 DXBC 布局取 chunk（RDEF/ISGN/OSGN/SHEX）。

反汇编只覆盖 D3D11 tokenized program（SHEX/SHDR），DXIL 容器（SFI0/RDAT/DXIL）
会被识别但不反汇编。所有指令都必须刚好消费完 SHEX 声明的 DWORD 数，
否则抛 ShaderError 并给出出错的 DWORD 下标与上下文。
"""

from __future__ import annotations

import hashlib
import re
import struct
import zlib
from dataclasses import dataclass, field

DXBC_MAGIC = b"DXBC"
_ZLIB_SECOND_BYTES = (0x01, 0x9C, 0xDA)
_COMPONENTS = "xyzw"


class ShaderError(RuntimeError):
    """着色器解析或反汇编失败。"""


# ---------------------------------------------------------------------------
# zlib 流扫描与 DXBC 定位
# ---------------------------------------------------------------------------

def iter_zlib_streams(data: bytes):
    """扫描 data 里所有自洽的 zlib 流，产出 (流偏移, 压缩长度, 解压结果)。"""
    pos, size = 0, len(data)
    while pos + 2 <= size:
        if data[pos] != 0x78 or data[pos + 1] not in _ZLIB_SECOND_BYTES:
            pos += 1
            continue
        engine = zlib.decompressobj()
        try:
            out = engine.decompress(data[pos:])
        except zlib.error:
            pos += 1
            continue
        if not engine.eof or len(out) < 64:
            pos += 1
            continue
        consumed = size - pos - len(engine.unused_data)
        if consumed <= 0:
            pos += 1
            continue
        yield pos, consumed, out
        pos += consumed


def _container_length(blob: bytes, start: int) -> int | None:
    """校验 start 处的 DXBC 容器长度，返回容器字节数（不可信则 None）。"""
    if start + 32 > len(blob):
        return None
    total = struct.unpack_from("<I", blob, start + 24)[0]
    if total < 32 or start + total > len(blob):
        return None
    return total


@dataclass
class Shader:
    """一个从材质包里捞出来的着色器。"""

    offset: int                 # zlib 流在宿主缓冲里的偏移
    source: str                 # "uexp" 或 "export:<名字>"
    data: bytes                 # 完整 DXBC 容器字节
    tail: bytes = b""           # 容器之后紧跟的 UE4 着色器参数表
    container: Dxbc = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self.container = Dxbc(self.data)

    @property
    def shader_type(self) -> str | None:
        return self.container.shader_type

    @property
    def version(self) -> str:
        return self.container.version

    def disassemble(self) -> str:
        return self.container.disassemble()

    def instructions(self):
        return self.container.instructions()

    @property
    def uniform_buffer_names(self) -> list[str]:
        """从容器后的参数表里取出 uniform buffer 名（按 cb 绑定顺序）。

        零售包剥离了 RDEF，反射数据拿不到；UE4 仍会把 uniform buffer 名
        （View/Primitive/MaterialCollection0/Material…）以 FString 形式留在
        着色器参数表里，这里按“长度前缀 + 可打印字符”把它们捞出来。
        """
        names: list[str] = []
        pos, size = 0, len(self.tail)
        while pos + 4 <= size:
            (length,) = struct.unpack_from("<i", self.tail, pos)
            if 1 <= length <= 64 and pos + 4 + length <= size:
                raw = self.tail[pos + 4:pos + 4 + length]
                if raw.endswith(b"\x00") and all(32 <= byte < 127 for byte in raw[:-1]):
                    names.append(raw[:-1].decode("latin1"))
                    pos += 4 + length
                    continue
            pos += 1
        return names


def disassemble(shader) -> str:
    """反汇编入口：接受 Shader、Dxbc 或 DXBC 容器字节。"""
    if isinstance(shader, Shader):
        return shader.disassemble()
    if isinstance(shader, Dxbc):
        return shader.disassemble()
    return Dxbc(shader).disassemble()


def find_shaders(package) -> list[Shader]:
    """扫描 package 的 uexp（以及每个导出的字节）里的 zlib 流，取出 DXBC 容器。

    同一个着色器可能在 uexp 与导出数据里重复出现，按 DXBC 内容哈希去重。
    """
    sources: list[tuple[str, bytes]] = [("uexp", package.uexp)]
    for export in getattr(package, "exports", ()):  # 导出数据可能不在 uexp 覆盖范围内
        try:
            sources.append((f"export:{export.name}", package.export_bytes(export)))
        except Exception:  # noqa: BLE001 - 导出越界时跳过，不影响 uexp 扫描
            continue

    found: dict[bytes, Shader] = {}
    for label, buffer in sources:
        if not buffer:
            continue
        for offset, _consumed, blob in iter_zlib_streams(buffer):
            pos = 0
            while True:
                index = blob.find(DXBC_MAGIC, pos)
                if index < 0:
                    break
                length = _container_length(blob, index)
                if length is None:
                    pos = index + 4
                    continue
                container = blob[index:index + length]
                digest = hashlib.sha1(container).digest()
                if digest not in found:
                    found[digest] = Shader(offset, label, container,
                                           blob[index + length:])
                pos = index + length
    return list(found.values())


# ---------------------------------------------------------------------------
# D3D11 token 表
# ---------------------------------------------------------------------------

PROGRAM_TYPES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}

# 操作码编号取自 D3D11 tokenized program format（与 d3dcompiler 反汇编结果逐条对照过）
OPCODES = {
    0x00: "add", 0x01: "and", 0x02: "break", 0x03: "breakc", 0x06: "case",
    0x07: "continue", 0x08: "continuec", 0x09: "cut", 0x0A: "default",
    0x0B: "ddx", 0x0C: "ddy", 0x0D: "discard", 0x0E: "div",
    0x0F: "dp2", 0x10: "dp3", 0x11: "dp4", 0x12: "else", 0x13: "emit",
    0x15: "endif", 0x16: "endloop", 0x17: "endswitch", 0x18: "eq",
    0x19: "exp", 0x1A: "frc", 0x1B: "ftoi", 0x1C: "ftou", 0x1D: "ge",
    0x1E: "iadd", 0x1F: "if", 0x20: "ieq", 0x21: "ige", 0x22: "ilt",
    0x23: "imad", 0x24: "imax", 0x25: "imin", 0x26: "imul", 0x27: "ine",
    0x28: "ineg", 0x29: "ishl", 0x2A: "ishr", 0x2B: "itof", 0x2C: "label",
    0x2D: "ld", 0x2E: "ld_ms", 0x2F: "log", 0x30: "loop", 0x31: "lt",
    0x32: "mad", 0x33: "min", 0x34: "max", 0x35: "dcl_immediateConstantBuffer",
    0x36: "mov", 0x37: "movc", 0x38: "mul", 0x39: "ne", 0x3A: "nop",
    0x3B: "not", 0x3C: "or", 0x3D: "resinfo", 0x3E: "ret", 0x3F: "retc",
    0x40: "round_ne", 0x41: "round_ni", 0x42: "round_pi", 0x43: "round_z",
    0x44: "rsq", 0x45: "sample", 0x46: "sample_c", 0x47: "sample_c_lz",
    0x48: "sample_l", 0x49: "sample_d", 0x4A: "sample_b", 0x4B: "sqrt",
    0x4C: "switch", 0x4D: "sincos", 0x4E: "udiv", 0x4F: "ult", 0x50: "uge",
    0x51: "umul", 0x53: "umax", 0x54: "umin", 0x55: "ushr", 0x56: "utof",
    0x57: "xor",
    0x58: "dcl_resource", 0x59: "dcl_constantbuffer", 0x5A: "dcl_sampler",
    0x5B: "dcl_index_range", 0x5C: "dcl_output_topology",
    0x5D: "dcl_input_primitive", 0x5E: "dcl_max_output_vertex_count",
    0x5F: "dcl_input", 0x60: "dcl_input_sgv", 0x61: "dcl_input_siv",
    0x62: "dcl_input_ps", 0x63: "dcl_input_ps_sgv", 0x64: "dcl_input_ps_siv",
    0x65: "dcl_output", 0x67: "dcl_output_siv", 0x68: "dcl_temps",
    0x69: "dcl_indexable_temp", 0x6A: "dcl_global_flags",
    0x6C: "lod", 0x6D: "gather4", 0x6E: "sample_pos", 0x6F: "sample_info",
    0x71: "hs_decls", 0x72: "hs_control_point_phase", 0x73: "hs_fork_phase",
    0x74: "hs_join_phase", 0x75: "emit_stream", 0x76: "cut_stream",
    0x78: "fcall", 0x79: "bufinfo",
    0x7A: "ddx_coarse", 0x7B: "ddx_fine", 0x7C: "ddy_coarse", 0x7D: "ddy_fine",
    0x7E: "gather4_c", 0x7F: "gather4_po", 0x80: "gather4_po_c",
    0x81: "rcp", 0x82: "f32tof16", 0x83: "f16tof32",
    0x86: "countbits", 0x87: "firstbit_hi", 0x88: "firstbit_lo",
    0x89: "firstbit_shi", 0x8A: "ubfe", 0x8B: "ibfe", 0x8C: "bfi",
    0x8D: "bfrev", 0x8E: "swapc", 0x8F: "dcl_stream",
    0x90: "dcl_function_body", 0x91: "dcl_function_table",
    0x92: "dcl_interface", 0x93: "dcl_input_control_point_count",
    0x94: "dcl_output_control_point_count", 0x95: "dcl_tessellator_domain",
    0x96: "dcl_tessellator_partitioning",
    0x97: "dcl_tessellator_output_primitive",
    0x98: "dcl_hs_max_tessfactor", 0x99: "dcl_hs_fork_phase_instance_count",
    0x9A: "dcl_hs_join_phase_instance_count", 0x9B: "dcl_thread_group",
    0x9C: "dcl_uav_typed", 0x9D: "dcl_uav_raw", 0x9E: "dcl_uav_structured",
    0x9F: "dcl_tgsm_raw", 0xA0: "dcl_tgsm_structured",
    0xA1: "dcl_resource_raw", 0xA2: "dcl_resource_structured",
    0xA3: "ld_uav_typed", 0xA4: "store_uav_typed", 0xA5: "ld_raw",
    0xA6: "store_raw", 0xA7: "ld_structured", 0xA8: "store_structured",
    0xA9: "atomic_and", 0xAA: "atomic_or", 0xAB: "atomic_xor",
    0xAC: "atomic_cmp_store", 0xAD: "atomic_iadd", 0xAE: "atomic_imax",
    0xAF: "atomic_imin", 0xB0: "atomic_umax", 0xB1: "atomic_umin",
    0xB2: "imm_atomic_alloc", 0xB3: "imm_atomic_consume",
    0xB4: "imm_atomic_iadd", 0xB5: "imm_atomic_and",
    0xB6: "imm_atomic_or", 0xB7: "imm_atomic_xor",
    0xB8: "imm_atomic_exch", 0xB9: "imm_atomic_cmp_exch",
    0xBA: "imm_atomic_imax", 0xBB: "imm_atomic_imin",
    0xBC: "imm_atomic_umax", 0xBD: "imm_atomic_umin",
    0xBE: "sync", 0xBF: "dadd", 0xC0: "dmax", 0xC1: "dmin",
    0xC2: "dmul", 0xC3: "deq", 0xC4: "dge", 0xC5: "dlt",
    0xC6: "dne", 0xC7: "dmov", 0xC8: "dmovc", 0xC9: "dtof",
    0xCA: "ftod", 0xCB: "eval_snapped", 0xCC: "eval_sample_index",
    0xCD: "eval_centroid", 0xCE: "dcl_function_body_count",
}

# 寄存器类型：0 r#、1 v#、2 o#、3 x#、4 立即数、6 s#、7 t#、8 cb#、9 icb、
# 16 m#（流输出）、30 u#
REGISTER_PREFIX = {0: "r", 1: "v", 2: "o", 3: "x", 4: "l", 6: "s", 7: "t",
                   8: "cb", 9: "icb", 16: "m", 30: "u"}

# 不带下标 DWORD 的寄存器：13 是 null，32 起是计算着色器的系统寄存器
_NO_INDEX_TYPES = frozenset({13}) | frozenset(range(32, 48))

# 实测：32 = vThreadID（SV_DispatchThreadID），36 = vThreadIDInGroupFlattened；
# 其余系统寄存器只给出占位名，避免猜错。
_SPECIAL_REGISTERS = {
    13: "null",
    32: "vThreadID",
    33: "vThreadGroupID",
    34: "vThreadIDInGroup",
    36: "vThreadIDInGroupFlattened",
}

# 声明类指令占用的操作数 DWORD 数（不含指令令牌）
DECL_SHAPES = {
    "dcl_global_flags": 0,
    "dcl_constantbuffer": 3,      # cb# + 索引 + 元素个数
    "dcl_sampler": 2,             # s# + 索引
    "dcl_resource": 3,            # t# + 索引 + 返回类型
    "dcl_resource_raw": 2,
    "dcl_resource_structured": 3,  # t# + 索引 + stride
    "dcl_uav_typed": 3,
    "dcl_uav_raw": 2,
    "dcl_uav_structured": 3,
    "dcl_input": 2,
    "dcl_input_ps": 2,
    "dcl_input_siv": 2,
    "dcl_input_sgv": 3,           # v# + 索引 + 语义
    "dcl_input_ps_sgv": 3,
    "dcl_input_ps_siv": 3,
    "dcl_output": 2,
    "dcl_output_sgv": 3,
    "dcl_output_siv": 3,
    "dcl_temps": 1,
    "dcl_indexable_temp": 3,
    "dcl_thread_group": 3,
    "dcl_max_output_vertex_count": 1,
    "dcl_output_topology": 1,
    "dcl_input_primitive": 1,
    "dcl_tessellator_domain": 1,
    "dcl_tessellator_partitioning": 1,
    "dcl_tessellator_output_primitive": 1,
    "dcl_input_control_point_count": 1,
    "dcl_output_control_point_count": 1,
}

# 普通指令的操作数个数（源操作数 + 目的操作数）
ARGUMENT_COUNTS = {
    "add": 3, "and": 3, "breakc": 1, "case": 1, "continue": 0, "continuec": 1,
    "cut": 0, "default": 0, "ddx": 2, "ddy": 2, "discard": 0, "div": 3,
    "dp2": 3, "dp3": 3, "dp4": 3, "else": 0, "emit": 0, "endif": 0,
    "endloop": 0, "endswitch": 0, "eq": 3, "exp": 2, "frc": 2, "ftoi": 2,
    "ftou": 2, "ge": 3, "iadd": 3, "if": 1, "ieq": 3, "ige": 3, "ilt": 3,
    "imad": 4, "imax": 3, "imin": 3, "imul": 3, "ine": 3, "ineg": 2,
    "ishl": 3, "ishr": 3, "itof": 2, "label": 1, "ld": 3, "ld_ms": 4,
    "log": 2, "loop": 0, "lt": 3, "mad": 4, "min": 3, "max": 3, "mov": 2,
    "movc": 4, "mul": 3, "ne": 3, "nop": 0, "not": 2, "or": 3, "resinfo": 3,
    "ret": 0, "retc": 1, "round_ne": 2, "round_ni": 2, "round_pi": 2,
    "round_z": 2, "rsq": 2, "sample": 4, "sample_c": 5, "sample_c_lz": 5,
    "sample_l": 5, "sample_d": 6, "sample_b": 5, "sqrt": 2, "switch": 1,
    "sincos": 3, "udiv": 4, "ult": 3, "uge": 3, "umul": 3, "umax": 3,
    "umin": 3, "ushr": 3, "utof": 2, "xor": 3, "ld_raw": 3,
    "ld_structured": 4, "store_raw": 2, "store_structured": 4,
    "store_uav_typed": 2, "ld_uav_typed": 2, "gather4": 5, "gather4_c": 6,
    "gather4_po": 6, "gather4_po_c": 7, "lod": 4, "sample_pos": 3,
    "sample_info": 2, "ddx_coarse": 2, "ddx_fine": 2, "ddy_coarse": 2,
    "ddy_fine": 2, "rcp": 2, "f32tof16": 2, "f16tof32": 2, "countbits": 2,
    "firstbit_hi": 2, "firstbit_lo": 2, "firstbit_shi": 2, "ubfe": 5,
    "ibfe": 5, "bfi": 5, "bfrev": 2, "swapc": 5, "sync": 0, "emit_stream": 1,
    "cut_stream": 1, "fcall": 1, "bufinfo": 2, "dcl_stream": 0,
}

# 采样指令（判断 PS 是否真正采样贴图时用）：sample/sample_c/_c_lz/_l/_d/_b、gather4 系列
SAMPLE_OPCODES = frozenset({0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
                            0x6D, 0x7E, 0x7F, 0x80})

# 条件指令：唯一操作数是源寄存器，不是目的
_CONDITION_OPS = frozenset({"if", "breakc", "continuec", "retc", "discard"})

# 带扩展令牌时惯用 *_indexable 后缀的操作码
_INDEXABLE_OPS = frozenset({0x2D, 0x2E, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
                            0x6C, 0x6D, 0x7E, 0x7F, 0x80, 0xA3, 0xA5, 0xA7})

_RESOURCE_DIMENSIONS = {
    1: "buffer", 2: "texture1d", 3: "texture2d", 4: "texture2darray",
    5: "texture3d", 6: "texturecube", 7: "texturecubearray",
    8: "raw_buffer", 9: "structured_buffer",
}

_SAMPLER_MODES = {0: "mode_default", 1: "mode_comparison", 2: "mode_mono"}

_SYSTEM_VALUES = {
    0: "undefined", 1: "position", 2: "clip_distance", 3: "cull_distance",
    4: "render_target_array_index", 5: "viewport_array_index", 6: "vertex_id",
    7: "primitive_id", 8: "instance_id", 9: "is_front_face",
    10: "sample_index", 11: "final_quad_u0_tessfactor",
    12: "final_quad_u1_tessfactor", 13: "final_quad_v0_tessfactor",
    14: "final_quad_v1_tessfactor", 15: "final_tri_u0_tessfactor",
    16: "final_tri_v0_tessfactor", 17: "final_tri_v1_tessfactor",
    18: "final_tri_v2_tessfactor", 19: "final_line_density_tessfactor",
    20: "final_line_detail_tessfactor",
}

_INTERPOLATION_MODES = {
    0: "", 1: "constant", 2: "linear", 3: "linear centroid",
    4: "linear noperspective", 5: "linear noperspective centroid",
    6: "linear sample", 7: "linear noperspective sample",
}

_COMPONENT_TYPES = {
    1: "uint", 2: "sint", 3: "float", 4: "float", 5: "float", 6: "mixed",
}


def _swizzle_text(bits: int) -> str:
    return "".join(_COMPONENTS[(bits >> (2 * i)) & 0x3] for i in range(4))


def _mask_text(bits: int) -> str:
    return "".join(_COMPONENTS[i] for i in range(4) if bits & (1 << i))


def _format_immediate(values: tuple[int, ...]) -> str:
    parts = []
    for raw in values:
        if raw == 0:
            parts.append("0")
            continue
        value = struct.unpack("<f", struct.pack("<I", raw))[0]
        if 1e-6 <= abs(value) <= 1e9 or value == 0.0:
            parts.append(f"{value:.6f}")
        else:
            parts.append(str(struct.unpack("<i", struct.pack("<I", raw))[0]))
    return "l(" + ", ".join(parts) + ")"


# ---------------------------------------------------------------------------
# RDEF / 签名
# ---------------------------------------------------------------------------

@dataclass
class ShaderVariable:
    name: str
    offset: int
    size: int
    members: list["ShaderVariable"] = field(default_factory=list)


@dataclass
class ConstantBuffer:
    name: str
    size: int
    type_id: int = 0
    variables: list[ShaderVariable] = field(default_factory=list)


@dataclass
class BoundResource:
    name: str
    dimension: str
    bind_point: int
    bind_count: int


@dataclass
class RdefInfo:
    buffers: list[ConstantBuffer] = field(default_factory=list)
    resources: list[BoundResource] = field(default_factory=list)

    def parameter_names(self) -> list[str]:
        names: list[str] = []
        for buffer in self.buffers:
            for variable in buffer.variables:
                names.append(variable.name)
                for member in variable.members:
                    names.append(f"{variable.name}.{member.name}")
        return names


@dataclass
class SignatureElement:
    name: str
    semantic_index: int
    sysvalue: int
    component_type: int
    register: int
    mask: int

    def text(self) -> str:
        sys = _SYSTEM_VALUES.get(self.sysvalue, str(self.sysvalue))
        comp = _COMPONENT_TYPES.get(self.component_type, str(self.component_type))
        return (f"{self.name}{self.semantic_index if self.semantic_index else ''}"
                f" {self.mask:02x} {self.register} {sys} {comp}")


def _read_cstring(payload: bytes, offset: int, limit: int | None = None) -> str:
    if offset < 0 or offset >= len(payload):
        raise ShaderError(f"RDEF 字符串偏移越界: {offset} / {len(payload)}")
    end = len(payload) if limit is None else min(limit, len(payload))
    stop = payload.find(b"\x00", offset, end)
    if stop < 0:
        stop = end
    return payload[offset:stop].decode("latin1", "replace")


def parse_rdef(payload: bytes) -> RdefInfo:
    """解析 RDEF chunk：常量缓冲（含成员名/偏移/大小）与绑定资源。"""
    info = RdefInfo()
    if len(payload) < 4:
        raise ShaderError("RDEF 过短")
    (buffer_count,) = struct.unpack_from("<I", payload, 0)
    pos = 4
    pending: list[tuple[ConstantBuffer, int, int]] = []
    for _ in range(buffer_count):
        (name_off, var_count, var_off, size, _flags, cb_type) = struct.unpack_from(
            "<IIIIII", payload, pos)
        pos += 24
        buffer = ConstantBuffer(_read_cstring(payload, name_off), size, cb_type)
        info.buffers.append(buffer)
        pending.append((buffer, var_off, var_count))

    if pos + 4 > len(payload):
        raise ShaderError("RDEF 缺少绑定资源计数")
    (resource_count,) = struct.unpack_from("<I", payload, pos)
    pos += 4
    for _ in range(resource_count):
        (name_off, dimension, bind_point, bind_count, _flags) = struct.unpack_from(
            "<IIIII", payload, pos)
        pos += 20
        info.resources.append(BoundResource(
            _read_cstring(payload, name_off),
            _RESOURCE_DIMENSIONS.get(dimension, f"dim{dimension}"),
            bind_point, bind_count))

    for buffer, var_off, var_count in pending:
        buffer.variables = _parse_variables(payload, var_off, var_count)
    return info


def _parse_variables(payload: bytes, offset: int, count: int) -> list[ShaderVariable]:
    variables: list[ShaderVariable] = []
    for _ in range(count):
        if offset + 32 > len(payload):
            raise ShaderError("RDEF 常量变量越界")
        (name_off, start, size, _flags, _default, _type_id,
         member_count, member_off) = struct.unpack_from("<IIIIIIII", payload, offset)
        offset += 32
        variable = ShaderVariable(_read_cstring(payload, name_off), start, size)
        if member_count:
            variable.members = _parse_members(payload, member_off, member_count)
        variables.append(variable)
    return variables


def _parse_members(payload: bytes, offset: int, count: int) -> list[ShaderVariable]:
    members: list[ShaderVariable] = []
    for _ in range(count):
        if offset + 12 > len(payload):
            raise ShaderError("RDEF 数组成员越界")
        name_off, _type_off, member_off = struct.unpack_from("<III", payload, offset)
        offset += 12
        members.append(ShaderVariable(_read_cstring(payload, name_off), member_off, 0))
    return members


def parse_signature(payload: bytes) -> list[SignatureElement]:
    """解析 ISGN/OSGN/OSG5：每项 24 字节，名字偏移相对 chunk 起点。"""
    if len(payload) < 8:
        raise ShaderError("签名 chunk 过短")
    (count,) = struct.unpack_from("<I", payload, 0)
    elements: list[SignatureElement] = []
    pos = 8
    for _ in range(count):
        if pos + 24 > len(payload):
            raise ShaderError("签名元素越界")
        name_off, sem, sysvalue, comp, reg, mask_rw = struct.unpack_from(
            "<IIIIII", payload, pos)
        pos += 24
        elements.append(SignatureElement(
            _read_cstring(payload, name_off), sem, sysvalue, comp, reg,
            mask_rw & 0xFF))
    return elements


# ---------------------------------------------------------------------------
# 指令
# ---------------------------------------------------------------------------

@dataclass
class Operand:
    text: str
    register_type: int = -1
    register_index: int = -1
    is_immediate: bool = False

    def __str__(self) -> str:
        return self.text


@dataclass
class Instruction:
    index: int                  # 指令在 SHEX 里的 DWORD 下标
    opcode: int
    name: str
    operands: list[Operand] = field(default_factory=list)

    @property
    def is_sample(self) -> bool:
        return self.opcode in SAMPLE_OPCODES

    def __str__(self) -> str:
        if self.operands:
            return f"{self.name} " + ", ".join(op.text for op in self.operands)
        return self.name


class Dxbc:
    """DXBC 容器（SM5 tokenized program）。"""

    def __init__(self, data: bytes):
        if len(data) < 32 or data[:4] != DXBC_MAGIC:
            raise ShaderError("不是 DXBC 容器")
        self.data = data
        self.total_size, count = struct.unpack_from("<II", data, 24)
        if self.total_size > len(data) or count > 64:
            raise ShaderError(f"DXBC 头异常: size={self.total_size} chunks={count}")
        offsets = struct.unpack_from(f"<{count}I", data, 32)
        self.chunks: list[tuple[str, bytes]] = []
        for offset in offsets:
            if offset + 8 > len(data):
                raise ShaderError(f"DXBC chunk 偏移越界: {offset}")
            fourcc = data[offset:offset + 4].decode("latin1")
            (size,) = struct.unpack_from("<I", data, offset + 4)
            if offset + 8 + size > len(data):
                raise ShaderError(f"DXBC chunk {fourcc} 长度越界")
            self.chunks.append((fourcc, data[offset + 8:offset + 8 + size]))
        self._rdef: RdefInfo | None = None

    # -- chunk 访问 -------------------------------------------------------
    def chunk(self, name: str) -> bytes | None:
        return next((payload for fourcc, payload in self.chunks
                     if fourcc == name), None)

    @property
    def chunk_names(self) -> list[str]:
        return [fourcc for fourcc, _ in self.chunks]

    @property
    def code(self) -> bytes | None:
        return self.chunk("SHEX") or self.chunk("SHDR")

    @property
    def is_dxil(self) -> bool:
        return self.chunk("DXIL") is not None

    # -- 程序头 -----------------------------------------------------------
    @property
    def version_token(self) -> int:
        code = self.code
        if code is None or len(code) < 4:
            return 0
        (token,) = struct.unpack_from("<I", code, 0)
        return token

    @property
    def program_type(self) -> int | None:
        if self.code is None:
            return None
        return (self.version_token >> 16) & 0xFFFF

    @property
    def shader_type(self) -> str | None:
        if self.code is None:
            return None
        return PROGRAM_TYPES.get(self.program_type, f"type{self.program_type}")

    @property
    def version(self) -> str:
        if self.code is None:
            return "dxil" if self.is_dxil else "unknown"
        token = self.version_token
        major = (token >> 4) & 0xF
        minor = token & 0xF
        return f"{self.shader_type}_{major}_{minor}"

    @property
    def dwords(self) -> list[int]:
        code = self.code
        if code is None:
            if self.is_dxil:
                raise ShaderError("DXIL 容器（SFI0/RDAT/DXIL）不支持反汇编")
            raise ShaderError("容器里没有 SHEX/SHDR")
        count = len(code) // 4
        return list(struct.unpack_from(f"<{count}I", code, 0))

    # -- RDEF / 签名 ------------------------------------------------------
    @property
    def rdef(self) -> RdefInfo:
        if self._rdef is None:
            payload = self.chunk("RDEF")
            if payload is None:
                self._rdef = RdefInfo()
            else:
                self._rdef = parse_rdef(payload)
        return self._rdef

    def signature(self, name: str) -> list[SignatureElement]:
        payload = self.chunk(name)
        if payload is None:
            return []
        return parse_signature(payload)

    @property
    def input_signature(self) -> list[SignatureElement]:
        return self.signature("ISGN")

    @property
    def output_signature(self) -> list[SignatureElement]:
        return self.signature("OSGN") or self.signature("OSG5")

    # -- 指令解析 ---------------------------------------------------------
    def instructions(self) -> list[Instruction]:
        dwords = self.dwords
        if len(dwords) < 2:
            raise ShaderError("SHEX 过短")
        declared = dwords[1]
        end = len(dwords)
        if declared != end:
            raise ShaderError(f"SHEX 声明 {declared} 个 DWORD，实际 {end} 个")

        result: list[Instruction] = []
        pos = 2
        while pos < end:
            token = dwords[pos]
            opcode = token & 0x7FF
            length = (token >> 24) & 0x7F
            name = OPCODES.get(opcode, f"op{opcode}")

            if opcode == 0x35:      # dcl_immediateConstantBuffer：长度字段为 0
                if pos + 2 > end:
                    raise ShaderError(f"第 {pos} 个 DWORD：立即数常量表被截断")
                count = dwords[pos + 1]
                stop = pos + 2 + count * 4
                if stop > end:
                    raise ShaderError(
                        f"第 {pos} 个 DWORD：立即数常量表 {count} 组越长 "
                        f"（需要 {stop} > {end}）")
                values = dwords[pos + 2:stop]
                inst = Instruction(pos, opcode, name)
                for k in range(count):
                    inst.operands.append(Operand(
                        _format_immediate(tuple(values[k * 4:k * 4 + 4])),
                        is_immediate=True))
                result.append(inst)
                pos = stop
                continue

            if length < 1:
                raise ShaderError(
                    f"第 {pos} 个 DWORD 处指令长度非法：令牌 0x{token:08X} "
                    f"opcode={opcode}({name}) length={length}")
            stop = pos + length
            if stop > end:
                raise ShaderError(
                    f"第 {pos} 个 DWORD 处指令越界：令牌 0x{token:08X} "
                    f"opcode={opcode}({name}) length={length}，需要 {stop} > {end}")

            inst = Instruction(pos, opcode, name)
            cursor = pos + 1

            if token & 0x80000000:  # 扩展操作码令牌：紧跟 2 个 DWORD（资源维度/数据类型）
                cursor += 2

            if name in DECL_SHAPES:
                cursor = self._parse_declaration(dwords, inst, cursor, stop)
            else:
                cursor = self._parse_regular(dwords, inst, cursor, stop, token)

            if cursor != stop:
                context = " ".join(f"0x{dwords[k]:08X}"
                                   for k in range(pos, min(stop, pos + 6)))
                raise ShaderError(
                    f"第 {pos} 个 DWORD 处解码失败：{name} 操作数读完后 "
                    f"剩余 {stop - cursor} 个 DWORD，上下文 [{context}]")

            # bit13 是普通指令的饱和修饰；声明类指令里这一段另有含义
            if token & (1 << 13) and opcode < 0x58:
                inst.name += "_sat"
            # 带扩展令牌的采样/载入指令按 D3D 习惯标成 *_indexable
            if token & 0x80000000 and opcode in _INDEXABLE_OPS:
                inst.name += "_indexable"
            result.append(inst)
            pos = stop
        return result

    def _parse_regular(self, dwords: list[int], inst: Instruction,
                       cursor: int, stop: int, token: int) -> int:
        name = inst.name
        if name in ("if", "breakc", "continuec", "retc", "discard"):
            inst.name = name + ("_nz" if token & (1 << 18) else "_z")
        count = ARGUMENT_COUNTS.get(name)
        # 条件指令的唯一操作数是“源”；imul/umul 可以带高低两个目的操作数
        while count is not None and count > 0:
            is_dst = name not in _CONDITION_OPS and (
                len(inst.operands) == 0 or
                (name in ("imul", "umul", "udiv") and len(inst.operands) == 1))
            operand, cursor = self._read_operand(dwords, cursor, stop, is_dst)
            inst.operands.append(operand)
            count -= 1
        # 未知操作数个数（或双目的操作数）时，一直读到指令末尾
        while cursor < stop:
            is_dst = name in ("imul", "umul", "udiv") and len(inst.operands) == 1
            operand, cursor = self._read_operand(dwords, cursor, stop, is_dst)
            inst.operands.append(operand)
        return cursor

    def _parse_declaration(self, dwords: list[int], inst: Instruction,
                           cursor: int, stop: int) -> int:
        """声明类指令：形状随程序类型/寄存器类型变化，按可用 DWORD 截断解析。"""
        name = inst.name
        token = dwords[inst.index]
        if name == "dcl_global_flags":
            flags = (token >> 11) & 0xFF
            inst.operands = [Operand("refactoringAllowed" if flags & 1 else
                                     f"flags=0x{flags:X}")]
            return cursor
        available = stop - cursor
        if available <= 0:
            return cursor
        first = dwords[cursor]
        first_type = (first >> 12) & 0xFF
        shape = DECL_SHAPES[name]
        if name in ("dcl_input", "dcl_input_ps", "dcl_input_siv"):
            if first_type >= 32:            # vThreadID 之类的系统寄存器没有下标
                shape = 1
            elif self.program_type == 2:    # 几何着色器的输入是 v[控制点数][元素]
                shape = 3
        shape = min(shape, available)
        raw = dwords[cursor:cursor + shape]
        cursor += shape

        if name == "dcl_temps":
            inst.operands = [Operand(str(raw[0]))]
            return cursor

        if name == "dcl_constantbuffer":
            index, count = raw[1], raw[2]
            inst.operands = [Operand(f"cb{index}[{count}]")]
            return cursor

        if name == "dcl_sampler":
            mode = (token >> 11) & 0xF
            inst.operands = [Operand(f"s{raw[1]}, "
                                    f"{_SAMPLER_MODES.get(mode, f'mode{mode}')}")]
            return cursor

        if name in ("dcl_resource", "dcl_resource_structured", "dcl_resource_raw",
                    "dcl_uav_typed", "dcl_uav_raw", "dcl_uav_structured"):
            text = f"{'u' if name.startswith('dcl_uav') else 't'}{raw[1]}"
            if name in ("dcl_resource_structured", "dcl_uav_structured"):
                text += f", stride={raw[2]}"
            elif len(raw) > 2:
                parts = [_COMPONENT_TYPES.get((raw[2] >> (4 * i)) & 0xF,
                                              str((raw[2] >> (4 * i)) & 0xF))
                         for i in range(4)]
                text += " (" + ",".join(parts) + ")"
            dimension = (token >> 11) & 0xF
            if dimension:
                text = f"{_RESOURCE_DIMENSIONS.get(dimension, f'dim{dimension}')} " + text
            inst.operands = [Operand(text)]
            return cursor

        if name in ("dcl_input", "dcl_input_ps", "dcl_input_siv"):
            if len(raw) >= 3:
                text = f"v[{raw[1]}][{raw[2]}]" + self._selection(raw[0], False)
            elif len(raw) == 2:
                text = self._register_text(raw[0], raw[1], False)
            else:
                text = self._register_text(raw[0], 0, False)
            mode = (token >> 11) & 0xF
            if name == "dcl_input_ps" and mode:
                text = _INTERPOLATION_MODES.get(mode, f"mode{mode}") + " " + text
            if name == "dcl_input_siv":
                text += ", " + _SYSTEM_VALUES.get(mode, str(mode))
            inst.operands = [Operand(text)]
            return cursor

        if name in ("dcl_input_sgv", "dcl_input_ps_sgv", "dcl_input_ps_siv"):
            text = self._register_text(raw[0], raw[1], False) if len(raw) > 2 \
                else self._register_text(raw[0], 0, False)
            semantic = raw[2] if len(raw) > 2 else (token >> 11) & 0xF
            text += ", " + _SYSTEM_VALUES.get(semantic, str(semantic))
            inst.operands = [Operand(text)]
            return cursor

        if name in ("dcl_output", "dcl_output_siv", "dcl_output_sgv"):
            text = self._register_text(raw[0], raw[1], True) if len(raw) > 1 \
                else self._register_text(raw[0], 0, True)
            if len(raw) > 2:
                text += ", " + _SYSTEM_VALUES.get(raw[2], str(raw[2]))
            inst.operands = [Operand(text)]
            return cursor

        # 其余声明：直接打印原始数值
        inst.operands = [Operand(str(value)) for value in raw]
        return cursor

    def _read_operand(self, dwords: list[int], cursor: int, stop: int,
                      is_dst: bool) -> tuple[Operand, int]:
        """读取一个操作数。

        D3D11 操作数编码（与 d3dcompiler 反汇编逐条对照得到）：
          * 令牌 bit31 置位时紧跟 1 个“修饰字”，其中 bit6-7：1=负号、2=绝对值、3=两者；
          * bit12-19 是寄存器类型，bit4-11 是 swizzle（源）或写掩码（目的），
            bit2-3 是分量选择方式（0=掩码、1=swizzle、2=标量），
            bit22-23 / bit25-26 分别是两个下标维度的寻址方式（0=绝对、2=相对、
            3=相对+偏移）；
          * 寄存器类型 4 是立即数：令牌 bit0-1 为 1 时占 1 个 DWORD，为 2 时占 4 个；
          * 常量缓冲 cb# 占两个索引 DWORD（寄存器号 + 元素偏移），
            x# 占两个下标维度，icb 占一个下标维度。
        """
        if cursor >= stop:
            raise ShaderError(f"第 {cursor} 个 DWORD：操作数缺失")
        token = dwords[cursor]
        cursor += 1
        modifier = ""
        if token & 0x80000000:
            if cursor >= stop:
                raise ShaderError(f"第 {cursor} 个 DWORD：扩展操作数令牌缺少修饰字")
            modifier = {1: "-", 2: "|", 3: "-|"}.get((dwords[cursor] >> 6) & 0x3, "")
            cursor += 1

        register_type = (token >> 12) & 0xFF
        if register_type == 4:      # 立即数
            count = 4 if (token & 0x3) == 2 else 1
            if cursor + count > stop:
                raise ShaderError(f"第 {cursor} 个 DWORD：立即数越界")
            values = tuple(dwords[cursor:cursor + count])
            text = _format_immediate(values)
            return Operand("-" + text if modifier == "-" else text,
                           register_type, -1, True), cursor + count

        base, index, cursor = self._operand_base(dwords, cursor, stop, token)
        text = base + self._selection(token, is_dst)
        if modifier:
            text = f"-|{text}|" if modifier == "-|" else (
                f"|{text}|" if modifier == "|" else "-" + text)
        return Operand(text, register_type, index), cursor

    def _operand_base(self, dwords: list[int], cursor: int, stop: int,
                      token: int) -> tuple[str, int, int]:
        """读取操作数下标并给出寄存器文本（不含 swizzle/掩码）。

        返回 (寄存器文本, 首下标值, 新游标)。
        """
        register_type = (token >> 12) & 0xFF
        if register_type in _NO_INDEX_TYPES:   # null / 系统寄存器：没有下标 DWORD
            return _SPECIAL_REGISTERS.get(register_type,
                                          f"type{register_type}"), -1, cursor
        if register_type == 8:      # cb#：两个索引 DWORD（寄存器号 + 元素偏移）
            if cursor + 2 > stop:
                raise ShaderError(f"第 {cursor} 个 DWORD：常量缓冲索引不完整")
            register, offset = dwords[cursor], dwords[cursor + 1]
            return f"cb{register}[{offset}]", register, cursor + 2

        # x# 与几何着色器的输入是二维下标，其余寄存器只有一维
        dimensions = 2 if (register_type == 3 or
                           (register_type == 1 and self.program_type == 2)) else 1
        parts: list[str] = []
        for dim in range(dimensions):
            mode = (token >> (22 + 3 * dim)) & 0x3
            text, cursor = self._read_index(dwords, cursor, stop, mode)
            parts.append(text)

        first = int(parts[0]) if parts[0].lstrip("-").isdigit() else -1
        if register_type == 3:      # x<寄存器号>[元素]
            return f"x{parts[0]}[{parts[-1]}]", first, cursor
        if register_type == 9:      # icb[下标]
            return f"icb[{parts[0]}]", first, cursor
        if dimensions == 2:         # 几何着色器输入：v[控制点数][元素]
            return f"v[{parts[0]}][{parts[1]}]", first, cursor
        return self._register_base(token, int(parts[0])), first, cursor

    def _read_index(self, dwords: list[int], cursor: int, stop: int,
                    mode: int) -> tuple[str, int]:
        if cursor >= stop:
            raise ShaderError(f"第 {cursor} 个 DWORD：下标缺失")
        if mode == 0:               # 绝对下标
            return str(dwords[cursor]), cursor + 1
        offset = 0
        if mode == 3:               # 相对 + 偏移：先一个“按字节打包”的偏移
            if cursor >= stop:
                raise ShaderError(f"第 {cursor} 个 DWORD：相对偏移缺失")
            packed = dwords[cursor]
            cursor += 1
            values = [struct.unpack("<b", struct.pack("<B", (packed >> (8 * i)) & 0xFF))[0]
                      for i in range(4)]
            offset = values[0] if len(set(values)) == 1 else 0
        if cursor + 2 > stop:
            raise ShaderError(f"第 {cursor} 个 DWORD：相对下标不完整")
        sub_token, sub_index = dwords[cursor], dwords[cursor + 1]
        cursor += 2
        base = self._register_base(sub_token, sub_index) + self._selection(sub_token, False)
        return (f"{base} + {offset}" if offset >= 0 else f"{base} - {-offset}"), cursor

    @staticmethod
    def _selection(token: int, is_dst: bool) -> str:
        bits = (token >> 4) & 0xFF
        if is_dst:
            return ("." + _mask_text(bits)) if bits else ""
        mode = (token >> 2) & 0x3
        if mode == 2:               # 标量：只取一个分量
            return "." + _COMPONENTS[bits & 0x3]
        if mode == 0:               # 掩码写法
            return ("." + _mask_text(bits)) if bits else ""
        return "." + _swizzle_text(bits)

    @staticmethod
    def _register_base(token: int, index: int) -> str:
        register_type = (token >> 12) & 0xFF
        prefix = REGISTER_PREFIX.get(register_type, f"type{register_type}")
        return f"{prefix}{index}"

    def _register_text(self, token: int, index: int, is_dst: bool) -> str:
        """声明指令用：按寄存器类型拼出 v#/o#/系统寄存器名。"""
        register_type = (token >> 12) & 0xFF
        selection = self._selection(token, is_dst)
        if register_type in _NO_INDEX_TYPES:
            return _SPECIAL_REGISTERS.get(register_type,
                                          f"type{register_type}") + selection
        return self._register_base(token, index) + selection

    # -- 反汇编文本 -------------------------------------------------------
    def declarations(self) -> dict[str, list[str]]:
        """汇总 dcl_* 声明，返回 cb/t/s/u 四组名字（零售包没有 RDEF 时靠它）。"""
        result: dict[str, list[str]] = {"cb": [], "t": [], "s": [], "u": []}
        for inst in self.instructions():
            if not inst.operands:
                continue
            text = inst.operands[0].text
            if inst.name == "dcl_constantbuffer":
                result["cb"].append(text)
            elif inst.name == "dcl_sampler":
                result["s"].append(text.split(",")[0].strip())
            elif inst.name.startswith(("dcl_resource", "dcl_uav")):
                match = re.search(r"\b([tu]\d+)\b", text)
                if match:
                    result["u" if match.group(1).startswith("u") else "t"].append(
                        match.group(1))
        return result

    def disassemble(self) -> str:
        if self.code is None:
            if self.is_dxil:
                raise ShaderError("DXIL 容器（SFI0/RDAT/DXIL）不支持反汇编")
            raise ShaderError("容器里没有 SHEX/SHDR")
        lines = [self.version]
        for inst in self.instructions():
            lines.append("  " + str(inst))
        return "\n".join(lines)

    def describe_rdef(self) -> str:
        info = self.rdef
        lines = [f"常量缓冲 {len(info.buffers)} 个，绑定资源 {len(info.resources)} 个"]
        for buffer in info.buffers:
            lines.append(f"  {buffer.name}  大小 {buffer.size} 字节")
            for variable in buffer.variables:
                lines.append(f"    {variable.name:<32} 偏移 {variable.offset:<5} "
                             f"大小 {variable.size}")
                for member in variable.members:
                    lines.append(f"      .{member.name}  偏移 {member.offset}")
        for resource in info.resources:
            lines.append(f"  资源 {resource.name}  类型 {resource.dimension}  "
                         f"绑定 t{resource.bind_point} 数量 {resource.bind_count}")
        return "\n".join(lines)
