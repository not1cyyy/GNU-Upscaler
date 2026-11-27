#!/usr/bin/env python3
"""
GNU-Upscaler Neural Look v2 Distillation Trainer & .nlmodel Pack Exporter

Trains the 12,572-parameter (3,143 x float4) ConvNeXt-Micro + 3D Bilateral Grid
model used in nl_neural_prior.hlsl on a physically-based multi-material 3D relighting,
intrinsic decomposition, and 6-class semantic surface dataset:
  Classes (softmax): skin, fabric, sand, soil, grass, hard_surface

Exports 4 hot-swappable .nlmodel packs:
  - photorealistic_pbr.nlmodel  (also baked into NL_DefaultModel.h)
  - path_traced_cinema.nlmodel
  - natural_daylight.nlmodel
  - filmic_noir.nlmodel
"""

import argparse
import math
import struct
import zlib
from pathlib import Path

import numpy as np

NL_MODEL_MAGIC = 0x4D4C4E47  # "GNLM"
NL_MODEL_VERSION = 2
NL_MODEL_FLOAT4_COUNT = 3143
NL_MODEL_FLOAT_COUNT = NL_MODEL_FLOAT4_COUNT * 4  # 12,572 floats

IN_CHANNELS = 20
MAT_CHANNELS = 16
NUM_SEM_CLASSES = 6  # 0:skin, 1:fabric, 2:sand, 3:soil, 4:grass, 5:hard_surface
SEM_NAMES = ["skin", "fabric", "sand", "soil", "grass", "hard_surface"]

REPO_ROOT = Path(__file__).resolve().parent.parent
NL_DIR = REPO_ROOT / "OptiScaler" / "OptiScaler" / "shaders" / "neural_look"
MODELS_DIR = NL_DIR / "models"
DEFAULT_HEADER_PATH = NL_DIR / "NL_DefaultModel.h"


def silu(x):
    s = 1.0 / (1.0 + np.exp(-np.clip(x, -20.0, 20.0)))
    return x * s, s


def silu_grad(x, s, dy):
    return dy * (s + x * s * (1.0 - s))


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -20.0, 20.0)))


def softmax_axis1(logits):
    m = np.max(logits, axis=1, keepdims=True)
    e = np.exp(np.clip(logits - m, -20.0, 20.0))
    return e / np.maximum(np.sum(e, axis=1, keepdims=True), 1e-8)


def depthwise_3x3_forward(x, w, b):
    N, C, H, W = x.shape
    xp = np.pad(x, ((0, 0), (0, 0), (1, 1), (1, 1)), mode="edge")
    out = np.broadcast_to(b[None, :, None, None], (N, C, H, W)).copy()
    k = 0
    for dy in range(3):
        for dx in range(3):
            out += xp[:, :, dy : dy + H, dx : dx + W] * w[:, k][None, :, None, None]
            k += 1
    return out


def depthwise_3x3_backward(x, w, dout):
    N, C, H, W = x.shape
    xp = np.pad(x, ((0, 0), (0, 0), (1, 1), (1, 1)), mode="edge")
    dxp = np.zeros_like(xp)
    dw = np.zeros_like(w)
    db = dout.sum(axis=(0, 2, 3))
    k = 0
    for dy in range(3):
        for dx in range(3):
            sl = xp[:, :, dy : dy + H, dx : dx + W]
            dw[:, k] = (sl * dout).sum(axis=(0, 2, 3))
            dxp[:, :, dy : dy + H, dx : dx + W] += dout * w[:, k][None, :, None, None]
            k += 1
    dx = dxp[:, :, 1 : 1 + H, 1 : 1 + W].copy()
    dx[:, :, 0, :] += dxp[:, :, 0, 1 : 1 + W]
    dx[:, :, -1, :] += dxp[:, :, -1, 1 : 1 + W]
    dx[:, :, :, 0] += dxp[:, :, 1 : 1 + H, 0]
    dx[:, :, :, -1] += dxp[:, :, 1 : 1 + H, -1]
    dx[:, :, 0, 0] += dxp[:, :, 0, 0]
    dx[:, :, 0, -1] += dxp[:, :, 0, -1]
    dx[:, :, -1, 0] += dxp[:, :, -1, 0]
    dx[:, :, -1, -1] += dxp[:, :, -1, -1]
    return dx, dw, db


def pw_forward(x, w, b):
    return np.einsum("oi,ni...->no...", w, x, optimize=True) + b.reshape(
        (1, -1) + (1,) * (x.ndim - 2)
    )


def pw_backward(x, w, dout):
    axes_sum = tuple(i for i in range(dout.ndim) if i != 1)
    db = dout.sum(axis=axes_sum)
    if x.ndim == 4:
        dw = np.einsum("nohw,nihw->oi", dout, x, optimize=True)
        dx = np.einsum("oi,nohw->nihw", w, dout, optimize=True)
    else:
        dw = np.einsum("no,ni->oi", dout, x, optimize=True)
        dx = np.einsum("oi,no->ni", w, dout, optimize=True)
    return dx, dw, db


class ConvNeXtMicroNet:
    def __init__(self, seed=42):
        rng = np.random.default_rng(seed)

        def init_w(out_c, in_c, scale=1.0):
            std = scale * math.sqrt(2.0 / max(in_c, 1))
            return rng.normal(0.0, std, size=(out_c, in_c)).astype(np.float32)

        def init_dw():
            w = rng.normal(0.0, 0.08, size=(32, 9)).astype(np.float32)
            w[:, 4] += 0.65
            return w

        self.params = {
            "W_stem": init_w(32, IN_CHANNELS),
            "B_stem": np.zeros(32, np.float32),
            "W_dw1": init_dw(),
            "B_dw1": np.zeros(32, np.float32),
            "W_pw1a": init_w(64, 32),
            "B_pw1a": np.zeros(64, np.float32),
            "W_pw1b": init_w(32, 64, 0.25),
            "B_pw1b": np.zeros(32, np.float32),
            "W_dw2": init_dw(),
            "B_dw2": np.zeros(32, np.float32),
            "W_pw2a": init_w(64, 40),
            "B_pw2a": np.zeros(64, np.float32),
            "W_pw2b": init_w(32, 64, 0.25),
            "B_pw2b": np.zeros(32, np.float32),
            "W_mat": init_w(MAT_CHANNELS, 32, 0.5),
            "B_mat": np.zeros(MAT_CHANNELS, np.float32),
            "W_bg1": init_w(32, 44),
            "B_bg1": np.zeros(32, np.float32),
            "W_bg2": init_w(12, 32, 0.2),
            "B_bg2": np.zeros(12, np.float32),
        }
        self.m = {k: np.zeros_like(v) for k, v in self.params.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.params.items()}
        self.step = 0

    def forward(self, x, luma_embed):
        p = self.params
        h0_pre = pw_forward(x, p["W_stem"], p["B_stem"])
        h0, s_h0 = silu(h0_pre)

        dw1 = depthwise_3x3_forward(h0, p["W_dw1"], p["B_dw1"])
        pw1a_pre = pw_forward(dw1, p["W_pw1a"], p["B_pw1a"])
        pw1a, s_pw1a = silu(pw1a_pre)
        pw1b = pw_forward(pw1a, p["W_pw1b"], p["B_pw1b"])
        h1 = h0 + pw1b

        g = h1[:, :8, :, :].mean(axis=(2, 3))
        g_broad = np.broadcast_to(g[:, :, None, None], (h1.shape[0], 8, h1.shape[2], h1.shape[3]))

        dw2 = depthwise_3x3_forward(h1, p["W_dw2"], p["B_dw2"])
        dw2_cat = np.concatenate([dw2, g_broad], axis=1)
        pw2a_pre = pw_forward(dw2_cat, p["W_pw2a"], p["B_pw2a"])
        pw2a, s_pw2a = silu(pw2a_pre)
        pw2b = pw_forward(pw2a, p["W_pw2b"], p["B_pw2b"])
        h2 = h1 + pw2b

        mat_logits = pw_forward(h2, p["W_mat"], p["B_mat"])  # (N, 16, H, W)
        mat_cont = sigmoid(mat_logits[:, 0:7, :, :])         # albedo(3), direct, rough, metal, wetness
        mat_sem = softmax_axis1(mat_logits[:, 7:13, :, :])   # skin, fabric, sand, soil, grass, hard
        mat_aux = sigmoid(mat_logits[:, 13:16, :, :])        # reserved

        h2_pool = h2.mean(axis=(2, 3))
        bg_in = np.concatenate([h2_pool, g, luma_embed], axis=1)
        bg1_pre = pw_forward(bg_in, p["W_bg1"], p["B_bg1"])
        bg1, s_bg1 = silu(bg1_pre)
        bg_pred = pw_forward(bg1, p["W_bg2"], p["B_bg2"])

        cache = (
            x, h0_pre, h0, s_h0, dw1, pw1a_pre, pw1a, s_pw1a, h1,
            g, dw2, dw2_cat, pw2a_pre, pw2a, s_pw2a, h2, mat_cont, mat_sem, mat_aux,
            h2_pool, bg_in, bg1_pre, bg1, s_bg1
        )
        return mat_cont, mat_sem, bg_pred, cache

    def backward_and_step(self, cache, d_mat_logits, d_bg, lr=2e-3, wd=1e-4):
        (
            x, h0_pre, h0, s_h0, dw1, pw1a_pre, pw1a, s_pw1a, h1,
            g, dw2, dw2_cat, pw2a_pre, pw2a, s_pw2a, h2, mat_cont, mat_sem, mat_aux,
            h2_pool, bg_in, bg1_pre, bg1, s_bg1
        ) = cache
        p = self.params
        grads = {}

        d_bg1, grads["W_bg2"], grads["B_bg2"] = pw_backward(bg1, p["W_bg2"], d_bg)
        d_bg1_pre = silu_grad(bg1_pre, s_bg1, d_bg1)
        d_bg_in, grads["W_bg1"], grads["B_bg1"] = pw_backward(bg_in, p["W_bg1"], d_bg1_pre)

        d_h2_pool = d_bg_in[:, :32]
        d_g_from_bg = d_bg_in[:, 32:40]

        d_h2_mat, grads["W_mat"], grads["B_mat"] = pw_backward(h2, p["W_mat"], d_mat_logits)

        H, W = h2.shape[2], h2.shape[3]
        d_h2 = d_h2_mat + d_h2_pool[:, :, None, None] / float(H * W)

        d_pw2a, grads["W_pw2b"], grads["B_pw2b"] = pw_backward(pw2a, p["W_pw2b"], d_h2)
        d_pw2a_pre = silu_grad(pw2a_pre, s_pw2a, d_pw2a)
        d_dw2_cat, grads["W_pw2a"], grads["B_pw2a"] = pw_backward(dw2_cat, p["W_pw2a"], d_pw2a_pre)

        d_dw2 = d_dw2_cat[:, :32, :, :]
        d_g_from_pw2 = d_dw2_cat[:, 32:40, :, :].sum(axis=(2, 3))
        d_h1_from_dw2, grads["W_dw2"], grads["B_dw2"] = depthwise_3x3_backward(h1, p["W_dw2"], d_dw2)

        d_h1 = d_h2 + d_h1_from_dw2
        d_h1[:, :8, :, :] += (d_g_from_bg + d_g_from_pw2)[:, :, None, None] / float(H * W)

        d_pw1a, grads["W_pw1b"], grads["B_pw1b"] = pw_backward(pw1a, p["W_pw1b"], d_h1)
        d_pw1a_pre = silu_grad(pw1a_pre, s_pw1a, d_pw1a)
        d_dw1, grads["W_pw1a"], grads["B_pw1a"] = pw_backward(dw1, p["W_pw1a"], d_pw1a_pre)
        d_h0_from_dw1, grads["W_dw1"], grads["B_dw1"] = depthwise_3x3_backward(h0, p["W_dw1"], d_dw1)

        d_h0 = d_h1 + d_h0_from_dw1
        d_h0_pre = silu_grad(h0_pre, s_h0, d_h0)
        _, grads["W_stem"], grads["B_stem"] = pw_backward(x, p["W_stem"], d_h0_pre)

        self.step += 1
        b1, b2, eps = 0.9, 0.999, 1e-8
        bc1 = 1.0 - b1 ** self.step
        bc2 = 1.0 - b2 ** self.step
        for k in self.params:
            g_k = np.clip(grads[k], -2.0, 2.0)
            self.m[k] = b1 * self.m[k] + (1.0 - b1) * g_k
            self.v[k] = b2 * self.v[k] + (1.0 - b2) * (g_k * g_k)
            m_hat = self.m[k] / bc1
            v_hat = self.v[k] / bc2
            self.params[k] -= lr * (m_hat / (np.sqrt(v_hat) + eps) + wd * self.params[k])

    def pack_weights_float4(self):
        """Pack all weights into exact (3143, 4) float32 layout matching nl_neural_prior.hlsl."""
        p = self.params
        chunks = []

        def pack_pw(w, b):
            out_c, in_c = w.shape
            w4 = w.reshape(out_c, in_c // 4, 4).reshape(-1, 4)
            b4 = b.reshape(out_c // 4, 4)
            chunks.append(w4)
            chunks.append(b4)

        pack_pw(p["W_stem"], p["B_stem"])  # 160 + 8 = 168

        chunks.append(p["W_dw1"].T.reshape(9 * 8, 4))  # 72
        chunks.append(p["B_dw1"].reshape(8, 4))        # 8

        pack_pw(p["W_pw1a"], p["B_pw1a"])  # 512 + 16 = 528
        pack_pw(p["W_pw1b"], p["B_pw1b"])  # 512 + 8  = 520

        chunks.append(p["W_dw2"].T.reshape(9 * 8, 4))  # 72
        chunks.append(p["B_dw2"].reshape(8, 4))        # 8

        pack_pw(p["W_pw2a"], p["B_pw2a"])  # 640 + 16 = 656
        pack_pw(p["W_pw2b"], p["B_pw2b"])  # 512 + 8  = 520
        pack_pw(p["W_mat"], p["B_mat"])    # 128 + 4  = 132
        pack_pw(p["W_bg1"], p["B_bg1"])    # 352 + 8  = 360
        pack_pw(p["W_bg2"], p["B_bg2"])    # 96 + 3   = 99

        packed = np.concatenate(chunks, axis=0).astype(np.float32)
        assert packed.shape == (NL_MODEL_FLOAT4_COUNT, 4), f"Got {packed.shape}"
        assert np.all(np.isfinite(packed)), "Non-finite weights detected!"
        return packed


def _sample_material_region(rng, mat_cls, H2, W2, xx2, yy2):
    """
    Synthesizes realistic half-res (2*H x 2*W) surface properties & micro-texture patterns
    for each of the 6 semantic classes, including hard negatives (earthy/olive fabrics,
    dry golden grass vs sand, wet mud vs dark asphalt).
    """
    albedo = np.zeros((H2, W2, 3), np.float32)
    rough = np.zeros((H2, W2), np.float32)
    metal = np.zeros((H2, W2), np.float32)
    wet = np.zeros((H2, W2), np.float32)
    micro_tex = np.zeros((H2, W2), np.float32)
    micro_dn = np.zeros((H2, W2, 2), np.float32)
    depth_jit = np.zeros((H2, W2), np.float32)

    if mat_cls == 0:  # Skin
        c = np.array([rng.uniform(0.42, 0.88), rng.uniform(0.24, 0.65), rng.uniform(0.16, 0.50)], np.float32)
        albedo[:] = c
        rough[:] = rng.uniform(0.32, 0.54)
        micro_tex[:] = rng.normal(0.0, 0.025, size=(H2, W2))
        world_up_dot = rng.uniform(-0.25, 0.55)
        island_bias = rng.uniform(0.25, 0.85)
        uv_y_center = rng.uniform(-0.6, 0.4)

    elif mat_cls == 1:  # Fabric (cotton, wool, velvet, denim, linen, earthy/khaki/olive cloth)
        variant = rng.integers(0, 4)
        if variant == 0:  # Rich dyed cloth (crimson, cobalt, emerald, plum, charcoal)
            c = rng.uniform(0.05, 0.78, size=3).astype(np.float32)
        elif variant == 1:  # Earthy/khaki/brown/olive cloth (hard negative vs sand/soil/grass!)
            c = np.array([rng.uniform(0.18, 0.58), rng.uniform(0.16, 0.50), rng.uniform(0.10, 0.35)], np.float32)
        elif variant == 2:  # Denim / slate blue
            c = np.array([rng.uniform(0.07, 0.22), rng.uniform(0.12, 0.34), rng.uniform(0.25, 0.58)], np.float32)
        else:  # Linen / off-white / dark wool
            v = rng.uniform(0.04, 0.72)
            c = np.array([v, v * rng.uniform(0.95, 1.05), v * rng.uniform(0.92, 1.06)], np.float32)

        albedo[:] = np.clip(c, 0.03, 0.88)
        rough[:] = rng.uniform(0.72, 0.96)
        wet[:] = rng.uniform(0.0, 0.06)

        # Cylindrical cloth fold waves + subtle directional weave
        fold_ang = rng.uniform(0.0, math.pi)
        fold_freq = rng.uniform(3.0, 8.5)
        phase = (xx2 * math.cos(fold_ang) + yy2 * math.sin(fold_ang)) * fold_freq + rng.uniform(0, 6.28)
        fold_grad = np.cos(phase) * rng.uniform(0.18, 0.42)
        micro_dn[:, :, 0] = fold_grad * math.cos(fold_ang)
        micro_dn[:, :, 1] = fold_grad * math.sin(fold_ang)

        weave_u = np.sin(xx2 * rng.uniform(18.0, 32.0)) * np.sin(yy2 * rng.uniform(18.0, 32.0))
        micro_tex[:] = weave_u * rng.uniform(0.03, 0.08) + rng.normal(0.0, 0.02, size=(H2, W2))
        world_up_dot = rng.uniform(-0.35, 0.58)
        island_bias = rng.uniform(0.22, 0.88)
        uv_y_center = rng.uniform(-0.5, 0.5)

    elif mat_cls == 2:  # Sand (desert dunes, beach sand, pale dust)
        c = np.array([rng.uniform(0.50, 0.85), rng.uniform(0.42, 0.74), rng.uniform(0.26, 0.55)], np.float32)
        # Occasional volcanic/grey beach sand
        if rng.random() < 0.20:
            v = rng.uniform(0.28, 0.62)
            c = np.array([v * 1.04, v, v * 0.92], np.float32)
        albedo[:] = np.clip(c, 0.15, 0.90)
        is_damp = rng.random() < 0.15
        wet[:] = rng.uniform(0.25, 0.55) if is_damp else rng.uniform(0.0, 0.05)
        rough[:] = rng.uniform(0.42, 0.68) if is_damp else rng.uniform(0.78, 0.96)

        # Smooth wind ripples (low curvature) + fine isotropic grain + positive quartz glint spikes!
        ripple_ang = rng.uniform(-0.5, 0.5)
        ripple = np.sin((xx2 * math.sin(ripple_ang) + yy2 * math.cos(ripple_ang)) * rng.uniform(4.0, 9.0))
        micro_dn[:, :, 0] = ripple * 0.05 * math.sin(ripple_ang)
        micro_dn[:, :, 1] = ripple * 0.05 * math.cos(ripple_ang)

        grain = rng.normal(0.0, 0.045, size=(H2, W2))
        glints = (rng.random(size=(H2, W2)) < 0.08).astype(np.float32) * rng.uniform(0.18, 0.55, size=(H2, W2))
        micro_tex[:] = grain + glints
        world_up_dot = rng.uniform(0.68, 0.99)
        island_bias = rng.uniform(0.0, 0.12)
        uv_y_center = rng.uniform(0.05, 0.85)

    elif mat_cls == 3:  # Soil / mud / dirt / earthy ground
        c = np.array([rng.uniform(0.10, 0.36), rng.uniform(0.07, 0.25), rng.uniform(0.04, 0.17)], np.float32)
        albedo[:] = c
        is_wet = rng.random() < 0.35
        wet[:] = rng.uniform(0.45, 0.95) if is_wet else rng.uniform(0.0, 0.18)
        rough[:] = rng.uniform(0.16, 0.44) if is_wet else rng.uniform(0.74, 0.96)
        if is_wet:
            albedo[:] *= rng.uniform(0.65, 0.85)

        # Clumpy multi-scale height perturbations + negative luma skewness (dark pores/crevices)
        micro_dn[:, :, 0] = rng.normal(0.0, 0.22, size=(H2, W2))
        micro_dn[:, :, 1] = rng.normal(0.0, 0.22, size=(H2, W2))
        clumps = rng.normal(0.0, 0.10, size=(H2, W2))
        pores = (rng.random(size=(H2, W2)) < 0.14).astype(np.float32) * rng.uniform(-0.48, -0.20, size=(H2, W2))
        micro_tex[:] = clumps + pores
        depth_jit[:] = rng.normal(0.0, 0.012, size=(H2, W2))
        world_up_dot = rng.uniform(0.48, 0.98)
        island_bias = rng.uniform(0.0, 0.18)
        uv_y_center = rng.uniform(0.0, 0.85)

    elif mat_cls == 4:  # Grass / vegetation / dry straw grass
        if rng.random() < 0.22:  # Dry golden/straw grass (hard negative vs sand!)
            c = np.array([rng.uniform(0.32, 0.62), rng.uniform(0.28, 0.54), rng.uniform(0.09, 0.22)], np.float32)
        else:  # Lush green/olive foliage
            c = np.array([rng.uniform(0.06, 0.28), rng.uniform(0.22, 0.68), rng.uniform(0.04, 0.20)], np.float32)
        albedo[:] = c
        rough[:] = rng.uniform(0.42, 0.74)
        wet[:] = rng.uniform(0.0, 0.20)

        # Strong vertical blade streaks -> horizontal luminance transitions (eV >> eH)
        blade_cols = rng.normal(0.0, 0.22, size=(1, W2))
        blade_wave = np.sin(xx2 * rng.uniform(22.0, 42.0) + rng.normal(0.0, 0.4, size=(H2, W2))) * 0.18
        micro_tex[:] = blade_cols + blade_wave + rng.normal(0.0, 0.06, size=(H2, W2))
        micro_dn[:, :, 0] = rng.normal(0.0, 0.28, size=(H2, W2))
        micro_dn[:, :, 1] = rng.normal(0.0, 0.16, size=(H2, W2))
        depth_jit[:] = rng.normal(0.0, 0.028, size=(H2, W2))
        world_up_dot = rng.uniform(0.20, 0.95)
        island_bias = rng.uniform(0.10, 0.45)
        uv_y_center = rng.uniform(-0.2, 0.80)

    else:  # 5: Hard surface (metal, concrete, stone wall, wet asphalt)
        is_metal = rng.random() < 0.50
        if is_metal:
            c = rng.uniform(0.10, 0.90, size=3).astype(np.float32)
            metal[:] = rng.uniform(0.75, 1.0)
            rough[:] = rng.uniform(0.06, 0.44)
        else:
            v = rng.uniform(0.08, 0.62)
            c = np.array([v, v * rng.uniform(0.95, 1.05), v * rng.uniform(0.95, 1.12)], np.float32)
            metal[:] = rng.uniform(0.0, 0.15)
            rough[:] = rng.uniform(0.32, 0.88)
        albedo[:] = np.clip(c, 0.04, 0.92)
        wet[:] = rng.uniform(0.0, 0.45) if not is_metal and rng.random() < 0.25 else 0.0
        micro_tex[:] = rng.normal(0.0, 0.05, size=(H2, W2))
        micro_dn[:, :, 0] = rng.normal(0.0, 0.04, size=(H2, W2))
        micro_dn[:, :, 1] = rng.normal(0.0, 0.04, size=(H2, W2))
        world_up_dot = rng.choice([rng.uniform(-0.15, 0.25), rng.uniform(0.85, 0.99)])
        island_bias = rng.uniform(0.05, 0.55)
        uv_y_center = rng.uniform(-0.7, 0.6)

    return albedo, rough, metal, wet, micro_tex, micro_dn, depth_jit, world_up_dot, island_bias, uv_y_center


def generate_pbr_batch(batch_size, rng, preset_style, H=16, W=16):
    """
    Synthesizes 2*H x 2*W half-res patches and applies the exact 2x2 nl_pyramid.hlsl
    feature reduction to produce (N, 20, H, W) eighth-res features + targets.
    """
    N = batch_size
    H2, W2 = H * 2, W * 2
    x_feat = np.zeros((N, IN_CHANNELS, H, W), dtype=np.float32)
    y_cont = np.zeros((N, 7, H, W), dtype=np.float32)
    y_sem = np.zeros((N, NUM_SEM_CLASSES, H, W), dtype=np.float32)
    luma_z = rng.uniform(0.0, 1.0, size=(N,)).astype(np.float32)
    luma_embed = np.stack(
        [luma_z, 2.0 * luma_z - 1.0, np.sin(math.pi * luma_z), np.cos(math.pi * luma_z)],
        axis=1,
    ).astype(np.float32)
    y_bg = np.zeros((N, 12), dtype=np.float32)

    yy2, xx2 = np.meshgrid(np.linspace(-1, 1, H2), np.linspace(-1, 1, W2), indexing="ij")

    for i in range(N):
        scene_type = rng.integers(0, 4)
        key_dir = rng.normal(0.0, 1.0, size=3).astype(np.float32)
        key_dir[2] = -abs(key_dir[2]) - 0.25
        key_dir /= np.linalg.norm(key_dir)

        if scene_type == 0:  # Sunny outdoor
            key_col = np.array([1.8, 1.65, 1.4], np.float32) * rng.uniform(0.8, 1.8)
            sky_col = np.array([0.28, 0.42, 0.65], np.float32) * rng.uniform(0.6, 1.2)
            base_depth = rng.uniform(4.0, 40.0)
        elif scene_type == 1:  # Dark indoor / cave
            key_col = np.array([1.3, 0.85, 0.50], np.float32) * rng.uniform(0.3, 0.9)
            sky_col = np.array([0.05, 0.06, 0.09], np.float32) * rng.uniform(0.5, 1.5)
            base_depth = rng.uniform(1.2, 10.0)
        elif scene_type == 2:  # Neon night
            key_col = np.array(rng.choice([[0.4, 1.4, 1.8], [1.7, 0.4, 1.2], [1.5, 0.9, 0.3]]), np.float32)
            sky_col = np.array([0.06, 0.08, 0.16], np.float32)
            base_depth = rng.uniform(2.0, 18.0)
        else:  # Overcast / soft portrait
            key_col = np.array([1.0, 1.0, 1.02], np.float32) * rng.uniform(0.7, 1.2)
            sky_col = np.array([0.35, 0.38, 0.42], np.float32)
            base_depth = rng.uniform(1.0, 8.0)

        split_ang = rng.uniform(0, math.pi)
        boundary2 = (xx2 * math.cos(split_ang) + yy2 * math.sin(split_ang)) > rng.uniform(-0.45, 0.45)
        mat_a = int(rng.integers(0, NUM_SEM_CLASSES))
        mat_b = int(rng.integers(0, NUM_SEM_CLASSES))

        a2, r2, m2, w2, tex2, dn2, dj2, upA, islA, uvyA = _sample_material_region(rng, mat_a, H2, W2, xx2, yy2)
        b_a, b_r, b_m, b_w, b_t, b_dn, b_dj, upB, islB, uvyB = _sample_material_region(rng, mat_b, H2, W2, xx2, yy2)

        maskB = boundary2
        albedo2 = np.where(maskB[:, :, None], b_a, a2)
        rough2 = np.where(maskB, b_r, r2)
        metal2 = np.where(maskB, b_m, m2)
        wet2 = np.where(maskB, b_w, w2)
        tex2 = np.where(maskB, b_t, tex2)
        dn2 = np.where(maskB[:, :, None], b_dn, dn2)
        dj2 = np.where(maskB, b_dj, dj2)

        sem2 = np.zeros((NUM_SEM_CLASSES, H2, W2), np.float32)
        sem2[mat_a, ~maskB] = 1.0
        sem2[mat_b, maskB] = 1.0

        world_up_dot2 = np.where(maskB, upB, upA).astype(np.float32)
        island2 = np.where(maskB, islB, islA).astype(np.float32)
        uv_y_off = float(np.where(maskB, uvyB, uvyA).mean())

        # Base curvature + micro normal perturbation
        nx2 = np.clip(xx2 * rng.uniform(-0.35, 0.35) + rng.uniform(-0.25, 0.25) + dn2[:, :, 0], -0.88, 0.88)
        ny2 = np.clip(yy2 * rng.uniform(-0.35, 0.35) + rng.uniform(-0.25, 0.25) + dn2[:, :, 1], -0.88, 0.88)
        nz2 = -np.sqrt(np.maximum(0.05, 1.0 - nx2 * nx2 - ny2 * ny2))

        depth2 = base_depth * (1.0 + 0.10 * xx2 + 0.08 * yy2 + 0.28 * island2 * maskB.astype(np.float32) + dj2)
        depth2 = np.maximum(depth2, 0.2)

        sky_mask2 = np.ones((H2, W2), np.float32)
        if rng.random() < 0.10:
            sky_mask2[maskB] = 0.0
            depth2[maskB] = 650.0
            sem2[:, maskB] = 0.0
            sem2[5, maskB] = 1.0

        ndotl2 = np.clip(nx2 * key_dir[0] + ny2 * key_dir[1] + nz2 * key_dir[2], 0.0, 1.0)
        shadow2 = np.clip(0.5 + 1.4 * (xx2 * key_dir[0] + yy2 * key_dir[1] + rng.uniform(-0.3, 0.3)), 0.06, 1.0)
        direct_irrad = (ndotl2 * shadow2)[:, :, None] * key_col[None, None, :]
        indirect_irrad = (0.5 + 0.5 * (-ny2[:, :, None])) * sky_col[None, None, :] + 0.04

        spec_f0 = 0.04 * (1.0 - metal2[:, :, None]) + albedo2 * metal2[:, :, None]
        eff_rough2 = np.clip(rough2 * (1.0 - 0.55 * wet2), 0.05, 1.0)
        spec_lobe = (ndotl2 ** np.clip((1.0 - eff_rough2) * 16.0, 1.5, 16.0))[:, :, None] * spec_f0 * shadow2[:, :, None] * key_col[None, None, :]
        diffuse_color = albedo2 * (1.0 - 0.85 * metal2[:, :, None])
        rgb2 = diffuse_color * (direct_irrad + indirect_irrad) + spec_lobe

        if np.any(sky_mask2 < 0.5):
            rgb2[sky_mask2 < 0.5] = sky_col * 2.2
            albedo2[sky_mask2 < 0.5] = np.clip(sky_col, 0.0, 1.0)
            rough2[sky_mask2 < 0.5] = 1.0
            metal2[sky_mask2 < 0.5] = 0.0
            wet2[sky_mask2 < 0.5] = 0.0

        rgb2 = np.maximum(rgb2 * (1.0 + tex2[:, :, None]), 0.0)
        luma2 = rgb2[:, :, 0] * 0.2126 + rgb2[:, :, 1] * 0.7152 + rgb2[:, :, 2] * 0.0722

        # Exact 2x2 nl_pyramid.hlsl feature reduction (H2,W2 -> H,W)
        l00 = luma2[0::2, 0::2]
        l10 = luma2[0::2, 1::2]
        l01 = luma2[1::2, 0::2]
        l11 = luma2[1::2, 1::2]
        l_mean = (l00 + l10 + l01 + l11) * 0.25
        tex_var = (np.abs(l00 - l_mean) + np.abs(l10 - l_mean) + np.abs(l01 - l_mean) + np.abs(l11 - l_mean)) * 0.25 / np.maximum(l_mean + 0.1, 0.1)

        eV = np.abs(l00 - l10) + np.abs(l01 - l11)
        eH = np.abs(l00 - l01) + np.abs(l10 - l11)
        aniso_vh = np.clip((eV - eH) / (eV + eH + 0.025), -1.0, 1.0)

        l_max = np.maximum(np.maximum(l00, l10), np.maximum(l01, l11))
        l_min = np.minimum(np.minimum(l00, l10), np.minimum(l01, l11))
        luma_skew = np.clip(((l_max - l_mean) - (l_mean - l_min)) / np.maximum(l_mean + 0.08, 0.08), -1.0, 1.0)

        nx_avg = (nx2[0::2, 0::2] + nx2[0::2, 1::2] + nx2[1::2, 0::2] + nx2[1::2, 1::2]) * 0.25
        ny_avg = (ny2[0::2, 0::2] + ny2[0::2, 1::2] + ny2[1::2, 0::2] + ny2[1::2, 1::2]) * 0.25
        nz_avg = -np.sqrt(np.maximum(0.0, 1.0 - nx_avg * nx_avg - ny_avg * ny_avg))

        norm_curv = np.clip((
            np.hypot(nx2[0::2, 0::2] - nx_avg, ny2[0::2, 0::2] - ny_avg) +
            np.hypot(nx2[0::2, 1::2] - nx_avg, ny2[0::2, 1::2] - ny_avg) +
            np.hypot(nx2[1::2, 0::2] - nx_avg, ny2[1::2, 0::2] - ny_avg) +
            np.hypot(nx2[1::2, 1::2] - nx_avg, ny2[1::2, 1::2] - ny_avg)
        ) * 1.25, 0.0, 1.0)

        d00, d10, d01, d11 = depth2[0::2, 0::2], depth2[0::2, 1::2], depth2[1::2, 0::2], depth2[1::2, 1::2]
        d_min = np.minimum(np.minimum(d00, d10), np.minimum(d01, d11))
        d_max = np.maximum(np.maximum(d00, d10), np.maximum(d01, d11))
        depth_island_2x2 = np.clip((d_max - d_min) / np.maximum(d_min * 0.12, 0.04), 0.0, 1.0)
        depth_island = np.clip(np.maximum(depth_island_2x2, island2[0::2, 0::2] * 0.45), 0.0, 1.0)
        depth_island[d_max >= 64000.0] = 0.0

        rgb = (rgb2[0::2, 0::2] + rgb2[0::2, 1::2] + rgb2[1::2, 0::2] + rgb2[1::2, 1::2]) * 0.25
        luma = rgb[:, :, 0] * 0.2126 + rgb[:, :, 1] * 0.7152 + rgb[:, :, 2] * 0.0722
        coarse_luma = float(np.mean(luma))
        inv_luma = 1.0 / np.maximum(luma, 0.02)

        sky_mask = np.minimum(np.minimum(sky_mask2[0::2, 0::2], sky_mask2[0::2, 1::2]),
                              np.minimum(sky_mask2[1::2, 0::2], sky_mask2[1::2, 1::2]))
        ndotl = np.clip(nx_avg * key_dir[0] + ny_avg * key_dir[1] + nz_avg * key_dir[2], 0.0, 1.0)
        world_up = np.clip(world_up_dot2[0::2, 0::2] + rng.normal(0.0, 0.04, size=(H, W)), -1.0, 1.0)

        rgb_sum = np.maximum(rgb[:, :, 0] + rgb[:, :, 1] + rgb[:, :, 2], 0.04)
        opp_rg = np.clip((rgb[:, :, 0] - rgb[:, :, 1]) / rgb_sum, -1.0, 1.0)
        opp_yb = np.clip((rgb[:, :, 0] + rgb[:, :, 1] - 2.0 * rgb[:, :, 2]) / rgb_sum, -1.5, 1.5)
        uv_y = np.clip(np.linspace(-0.3, 0.3, H)[:, None] + uv_y_off, -1.0, 1.0)
        uv_y = np.broadcast_to(uv_y, (H, W))

        x_feat[i, 0] = np.log2(1.0 + luma) * 0.25
        x_feat[i, 1] = np.clip(rgb[:, :, 0] * inv_luma, 0.0, 3.0)
        x_feat[i, 2] = np.clip(rgb[:, :, 1] * inv_luma, 0.0, 3.0)
        x_feat[i, 3] = np.clip(rgb[:, :, 2] * inv_luma, 0.0, 3.0)
        x_feat[i, 4] = np.clip(np.log2(1.0 + np.minimum(d_min, 1024.0)) * 0.1, 0.0, 1.0)
        x_feat[i, 5] = nx_avg
        x_feat[i, 6] = ny_avg
        x_feat[i, 7] = np.abs(nz_avg)
        x_feat[i, 8] = np.clip(tex_var * 4.0, 0.0, 1.0)
        x_feat[i, 9] = sky_mask
        x_feat[i, 10] = np.clip(luma / max(coarse_luma + 0.05, 0.05) - 1.0, -1.0, 2.0)
        x_feat[i, 11] = np.clip(ndotl * 0.5 + 0.5, 0.0, 1.0)
        x_feat[i, 12] = world_up
        x_feat[i, 13] = aniso_vh
        x_feat[i, 14] = norm_curv
        x_feat[i, 15] = luma_skew
        x_feat[i, 16] = depth_island
        x_feat[i, 17] = opp_rg
        x_feat[i, 18] = opp_yb
        x_feat[i, 19] = uv_y

        albedo = (albedo2[0::2, 0::2] + albedo2[0::2, 1::2] + albedo2[1::2, 0::2] + albedo2[1::2, 1::2]) * 0.25
        rough = (rough2[0::2, 0::2] + rough2[0::2, 1::2] + rough2[1::2, 0::2] + rough2[1::2, 1::2]) * 0.25
        metal = (metal2[0::2, 0::2] + metal2[0::2, 1::2] + metal2[1::2, 0::2] + metal2[1::2, 1::2]) * 0.25
        wet = (wet2[0::2, 0::2] + wet2[0::2, 1::2] + wet2[1::2, 0::2] + wet2[1::2, 1::2]) * 0.25

        dir_e = np.sum(direct_irrad[0::2, 0::2], axis=2) * (1.0 - 0.5 * metal)
        tot_e = dir_e + np.sum(indirect_irrad[0::2, 0::2], axis=2) + 1e-4
        direct_mask = np.clip(dir_e / tot_e, 0.0, 1.0) * sky_mask

        y_cont[i, 0:3] = np.transpose(np.clip(albedo, 0.0, 1.0), (2, 0, 1))
        y_cont[i, 3] = direct_mask
        y_cont[i, 4] = np.clip(rough, 0.0, 1.0)
        y_cont[i, 5] = np.clip(metal, 0.0, 1.0)
        y_cont[i, 6] = np.clip(wet, 0.0, 1.0)

        sem = (sem2[:, 0::2, 0::2] + sem2[:, 0::2, 1::2] + sem2[:, 1::2, 0::2] + sem2[:, 1::2, 1::2]) * 0.25
        sem /= np.maximum(np.sum(sem, axis=0, keepdims=True), 1e-6)
        y_sem[i] = sem

        # Bilateral grid target per look style
        lz = luma_z[i]
        mean_skin = float(np.mean(sem[0]))
        mean_grass = float(np.mean(sem[4]))
        mean_metal = float(np.mean(metal))
        mean_var = float(np.mean(tex_var))
        shadow_zone = max(0.0, 1.0 - lz * 2.2)
        high_zone = max(0.0, (lz - 0.6) * 2.5)
        mid_zone = math.exp(-((lz - 0.42) ** 2) / 0.08)

        if preset_style == "photorealistic_pbr":
            gain = np.array([1.0 + 0.05 * mid_zone - 0.04 * high_zone,
                             1.0 + 0.04 * mid_zone - 0.04 * high_zone,
                             1.0 + 0.03 * mid_zone - 0.03 * high_zone])
            gain += np.array([0.04, 0.01, -0.02]) * mean_skin * mid_zone
            shadow_lift = 0.03 * shadow_zone * (1.0 - 0.4 * mean_metal)
            off = np.zeros(3, dtype=np.float32)
            detail = 0.16 * (0.4 + 0.6 * mean_var) * (1.0 - 0.45 * mean_skin) * (1.0 - 0.55 * mean_grass)
            chroma = 0.10 * mid_zone + 0.06 * mean_grass
            spec_boost = 0.14 * mean_metal + 0.05 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.16 * high_zone
        elif preset_style == "path_traced_cinema":
            gain = np.array([1.05 * mid_zone + 0.95 * (1.0 - mid_zone),
                             1.02 * mid_zone + 0.96 * (1.0 - mid_zone),
                             0.97 * mid_zone + 1.02 * (1.0 - mid_zone)])
            gain += np.array([0.05, 0.02, -0.03]) * mean_skin
            shadow_lift = 0.02 * shadow_zone - 0.01 * (1.0 - shadow_zone)
            off = np.zeros(3, dtype=np.float32)
            detail = 0.20 * (0.45 + 0.55 * mean_var) * (1.0 - 0.50 * mean_skin) * (1.0 - 0.55 * mean_grass)
            chroma = 0.18 * mid_zone + 0.08 * mean_grass
            spec_boost = 0.20 * mean_metal + 0.08 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.24 * high_zone
        elif preset_style == "natural_daylight":
            gain = np.array([1.02, 1.03, 1.04]) - 0.05 * high_zone
            shadow_lift = 0.04 * shadow_zone
            off = np.zeros(3, dtype=np.float32)
            detail = 0.13 * (0.5 + 0.5 * mean_var) * (1.0 - 0.40 * mean_skin) * (1.0 - 0.50 * mean_grass)
            chroma = 0.08 * mid_zone + 0.06 * mean_grass
            spec_boost = 0.10 * mean_metal + 0.04 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.14 * high_zone
        else:  # filmic_noir
            gain = np.array([0.96 + 0.10 * mid_zone, 0.95 + 0.08 * mid_zone, 0.98 + 0.07 * mid_zone])
            shadow_lift = -0.04 * shadow_zone
            off = np.zeros(3, dtype=np.float32)
            detail = 0.22 * (0.4 + 0.6 * mean_var) * (1.0 - 0.35 * mean_skin) * (1.0 - 0.50 * mean_grass)
            chroma = -0.08 + 0.08 * mean_skin
            spec_boost = 0.24 * mean_metal + 0.10 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.30 * high_zone

        y_bg[i, 0:3] = gain - 1.0
        y_bg[i, 3] = shadow_lift
        y_bg[i, 4:7] = off
        y_bg[i, 7] = detail
        y_bg[i, 8] = chroma
        y_bg[i, 9] = spec_boost
        y_bg[i, 10] = hl_roll
        y_bg[i, 11] = float(np.mean(sky_mask))

    return x_feat, luma_embed, y_cont, y_sem, y_bg


def write_nlmodel(path: Path, name: str, weights_f4: np.ndarray):
    payload = np.ascontiguousarray(weights_f4, dtype=np.float32).tobytes()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    name_bytes = name.encode("utf-8")[:31].ljust(32, b"\x00")
    header = struct.pack(
        "<IIII32s4I",
        NL_MODEL_MAGIC,
        NL_MODEL_VERSION,
        NL_MODEL_FLOAT4_COUNT,
        crc,
        name_bytes,
        0,
        0,
        0,
        0,
    )
    assert len(header) == 64
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + payload)


def verify_nlmodel(path: Path):
    data = path.read_bytes()
    assert len(data) == 64 + NL_MODEL_FLOAT_COUNT * 4, f"Bad size on {path}: {len(data)}"
    magic, version, num_f4, crc, name_b, r0, r1, r2, r3 = struct.unpack("<IIII32s4I", data[:64])
    assert magic == NL_MODEL_MAGIC, f"Bad magic in {path}"
    assert version == NL_MODEL_VERSION, f"Bad version in {path}"
    assert num_f4 == NL_MODEL_FLOAT4_COUNT, f"Bad float4 count in {path}"
    payload = data[64:]
    actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
    assert actual_crc == crc, f"CRC mismatch in {path}: {actual_crc:#x} != {crc:#x}"
    arr = np.frombuffer(payload, dtype=np.float32)
    assert np.all(np.isfinite(arr)), f"Non-finite weights in {path}"
    name_str = name_b.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    return name_str, arr.reshape(NL_MODEL_FLOAT4_COUNT, 4)


def emit_default_header(path: Path, weights_f4: np.ndarray):
    lines = [
        "#pragma once",
        "#include <array>",
        "#include <cstdint>",
        "",
        f"inline constexpr uint32_t kNLDefaultModelFloat4Count = {NL_MODEL_FLOAT4_COUNT}u;",
        "",
        f"inline const std::array<float, {NL_MODEL_FLOAT_COUNT}> kNLDefaultModelWeights = {{",
    ]
    flat = weights_f4.reshape(-1)
    for idx in range(0, len(flat), 8):
        chunk = ", ".join(f"{float(v):.9g}f" for v in flat[idx : idx + 8])
        comma = "," if idx + 8 < len(flat) else ""
        lines.append(f"    {chunk}{comma}")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def train_pack(style_name: str, epochs: int, batch_size: int, seed: int):
    rng = np.random.default_rng(seed)
    net = ConvNeXtMicroNet(seed=seed)

    val_x, val_le, val_yc, val_ys, val_ybg = generate_pbr_batch(96, np.random.default_rng(seed + 999), style_name)

    def eval_loss(x, le, yc, ys, ybg):
        pc, ps, pbg, _ = net.forward(x, le)
        l_cont = float(np.mean((pc - yc) ** 2))
        l_sem = float(-np.mean(np.sum(ys * np.log(np.maximum(ps, 1e-6)), axis=1)))
        l_bg = float(np.mean((pbg - ybg) ** 2))
        pred_cls = np.argmax(ps, axis=1)
        gt_cls = np.argmax(ys, axis=1)
        acc = float(np.mean(pred_cls == gt_cls))
        per_cls = []
        for c_idx in range(NUM_SEM_CLASSES):
            m = (gt_cls == c_idx)
            per_cls.append(float(np.mean(pred_cls[m] == c_idx)) if np.any(m) else 1.0)
        return l_cont + 0.35 * l_sem + l_bg, acc, per_cls

    init_loss, init_acc, _ = eval_loss(val_x, val_le, val_yc, val_ys, val_ybg)
    print(f"[{style_name}] initial val_loss={init_loss:.5f} sem_acc={init_acc*100:.1f}%")

    steps_per_epoch = 28
    total_steps = epochs * steps_per_epoch
    base_lr = 4.5e-3

    for ep in range(epochs):
        for st in range(steps_per_epoch):
            step_idx = ep * steps_per_epoch + st
            lr = base_lr * (0.08 + 0.92 * 0.5 * (1.0 + math.cos(math.pi * step_idx / max(total_steps, 1))))
            bx, ble, byc, bys, bybg = generate_pbr_batch(batch_size, rng, style_name)
            pc, ps, pbg, cache = net.forward(bx, ble)

            n_pix = float(batch_size * bx.shape[2] * bx.shape[3])
            d_mat_logits = np.zeros((batch_size, MAT_CHANNELS, bx.shape[2], bx.shape[3]), dtype=np.float32)
            # Continuous MSE through sigmoid
            d_mat_logits[:, 0:7, :, :] = (2.0 * (pc - byc) / (7.0 * n_pix)) * pc * (1.0 - pc)
            # Softmax cross-entropy gradient (weighted 0.45 for crisp semantic separation)
            d_mat_logits[:, 7:13, :, :] = 0.45 * (ps - bys) / n_pix
            d_bg = (2.0 * (pbg - bybg) / (12.0 * float(batch_size)))

            net.backward_and_step(cache, d_mat_logits, d_bg, lr=lr, wd=4e-5)

        if (ep + 1) % max(1, epochs // 3) == 0 or ep == epochs - 1:
            v_loss, v_acc, per_cls = eval_loss(val_x, val_le, val_yc, val_ys, val_ybg)
            cls_str = " ".join(f"{SEM_NAMES[k]}={per_cls[k]*100:.0f}%" for k in range(NUM_SEM_CLASSES))
            print(f"  epoch {ep + 1:02d}/{epochs}: val_loss={v_loss:.5f} sem_acc={v_acc*100:.1f}% ({cls_str})")

    final_loss, final_acc, _ = eval_loss(val_x, val_le, val_yc, val_ys, val_ybg)
    assert final_loss < init_loss * 0.45, f"Training did not converge sufficiently: {init_loss} -> {final_loss}"
    assert final_acc >= 0.85, f"Semantic accuracy too low: {final_acc*100:.1f}%"
    return net.pack_weights_float4()


def main():
    parser = argparse.ArgumentParser(description="Train & verify GNU-Upscaler Neural Look v2 models")
    parser.add_argument("--epochs", type=int, default=24)
    parser.add_argument("--batch-size", type=int, default=24)
    parser.add_argument("--verify", action="store_true", help="Verify existing .nlmodel packs and header")
    args = parser.parse_args()

    packs = [
        ("photorealistic_pbr", 101),
        ("path_traced_cinema", 202),
        ("natural_daylight", 303),
        ("filmic_noir", 404),
    ]

    if args.verify:
        for idx, (pack_name, _) in enumerate(packs):
            p = MODELS_DIR / f"{pack_name}.nlmodel"
            name_str, arr = verify_nlmodel(p)
            print(f"VERIFIED {p.name}: name='{name_str}', shape={arr.shape}, range=[{arr.min():.3f}, {arr.max():.3f}]")
            if idx == 0:
                hdr_txt = DEFAULT_HEADER_PATH.read_text(encoding="utf-8")
                assert f"kNLDefaultModelFloat4Count = {NL_MODEL_FLOAT4_COUNT}u;" in hdr_txt
                assert f"std::array<float, {NL_MODEL_FLOAT_COUNT}>" in hdr_txt
        print("All v2 .nlmodel packs and NL_DefaultModel.h verified successfully.")
        return

    for idx, (pack_name, seed) in enumerate(packs):
        weights_f4 = train_pack(pack_name, epochs=args.epochs, batch_size=args.batch_size, seed=seed)
        out_path = MODELS_DIR / f"{pack_name}.nlmodel"
        write_nlmodel(out_path, pack_name, weights_f4)
        verify_nlmodel(out_path)
        print(f"Saved {out_path}")
        if idx == 0:
            emit_default_header(DEFAULT_HEADER_PATH, weights_f4)
            print(f"Emitted {DEFAULT_HEADER_PATH}")


if __name__ == "__main__":
    main()
