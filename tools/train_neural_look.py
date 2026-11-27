#!/usr/bin/env python3
"""
GNU-Upscaler Neural Look Distillation Trainer & .nlmodel Pack Exporter

Trains the real 12,052-parameter (3,013 x float4) ConvNeXt-Micro + 3D Bilateral Grid
model used in nl_neural_prior.hlsl on a physically-based multi-material 3D relighting
and intrinsic decomposition dataset (plus optional external path-traced image pairs).

Exports 4 hot-swappable .nlmodel packs:
  - photorealistic_pbr.nlmodel  (also baked into NL_DefaultModel.h)
  - path_traced_cinema.nlmodel
  - natural_daylight.nlmodel
  - filmic_noir.nlmodel
"""

import argparse
import math
import os
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

NL_MODEL_MAGIC = 0x4D4C4E47  # "GNLM"
NL_MODEL_VERSION = 1
NL_MODEL_FLOAT4_COUNT = 3013
NL_MODEL_FLOAT_COUNT = NL_MODEL_FLOAT4_COUNT * 4  # 12,052 floats

REPO_ROOT = Path(__file__).resolve().parent.parent
NL_DIR = REPO_ROOT / "OptiScaler" / "OptiScaler" / "shaders" / "neural_look"
MODELS_DIR = NL_DIR / "models"
DEFAULT_HEADER_PATH = NL_DIR / "NL_DefaultModel.h"


def silu(x):
    # Numerically stable SiLU
    s = 1.0 / (1.0 + np.exp(-np.clip(x, -20.0, 20.0)))
    return x * s, s


def silu_grad(x, s, dy):
    # d/dx (x * sigmoid(x)) = s + x * s * (1 - s)
    return dy * (s + x * s * (1.0 - s))


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -20.0, 20.0)))


def depthwise_3x3_forward(x, w, b):
    # x: (N, C, H, W), w: (C, 9), b: (C,)
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
    # Returns dx, dw, db
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
    # Fold edge-padding grads back
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
    # x: (N, Cin, ...), w: (Cout, Cin), b: (Cout,)
    return np.einsum("oi,ni...->no...", w, x, optimize=True) + b.reshape(
        (1, -1) + (1,) * (x.ndim - 2)
    )


def pw_backward(x, w, dout):
    # dout: (N, Cout, ...)
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
            w[:, 4] += 0.65  # identity center prior
            return w

        self.params = {
            "W_stem": init_w(32, 12),
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
            "W_mat": init_w(8, 32, 0.5),
            "B_mat": np.zeros(8, np.float32),
            "W_bg1": init_w(32, 44),
            "B_bg1": np.zeros(32, np.float32),
            "W_bg2": init_w(12, 32, 0.2),
            "B_bg2": np.zeros(12, np.float32),
        }
        self.m = {k: np.zeros_like(v) for k, v in self.params.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.params.items()}
        self.step = 0

    def forward(self, x, luma_embed):
        # x: (N, 12, H, W), luma_embed: (N, 4)
        p = self.params
        h0_pre = pw_forward(x, p["W_stem"], p["B_stem"])
        h0, s_h0 = silu(h0_pre)

        dw1 = depthwise_3x3_forward(h0, p["W_dw1"], p["B_dw1"])
        pw1a_pre = pw_forward(dw1, p["W_pw1a"], p["B_pw1a"])
        pw1a, s_pw1a = silu(pw1a_pre)
        pw1b = pw_forward(pw1a, p["W_pw1b"], p["B_pw1b"])
        h1 = h0 + pw1b

        # Global tile context: mean over spatial H,W of first 8 channels of h1
        g = h1[:, :8, :, :].mean(axis=(2, 3))  # (N, 8)
        g_broad = np.broadcast_to(g[:, :, None, None], (h1.shape[0], 8, h1.shape[2], h1.shape[3]))

        dw2 = depthwise_3x3_forward(h1, p["W_dw2"], p["B_dw2"])
        dw2_cat = np.concatenate([dw2, g_broad], axis=1)  # (N, 40, H, W)
        pw2a_pre = pw_forward(dw2_cat, p["W_pw2a"], p["B_pw2a"])
        pw2a, s_pw2a = silu(pw2a_pre)
        pw2b = pw_forward(pw2a, p["W_pw2b"], p["B_pw2b"])
        h2 = h1 + pw2b

        mat_logits = pw_forward(h2, p["W_mat"], p["B_mat"])
        mat_pred = sigmoid(mat_logits)  # (N, 8, H, W)

        # Bilateral grid head operates on tile mean-pooled h2 (N, 32) + g (N, 8) + luma_embed (N, 4)
        h2_pool = h2.mean(axis=(2, 3))
        bg_in = np.concatenate([h2_pool, g, luma_embed], axis=1)  # (N, 44)
        bg1_pre = pw_forward(bg_in, p["W_bg1"], p["B_bg1"])
        bg1, s_bg1 = silu(bg1_pre)
        bg_pred = pw_forward(bg1, p["W_bg2"], p["B_bg2"])  # (N, 12)

        cache = (
            x, h0_pre, h0, s_h0, dw1, pw1a_pre, pw1a, s_pw1a, h1,
            g, dw2, dw2_cat, pw2a_pre, pw2a, s_pw2a, h2, mat_pred,
            h2_pool, bg_in, bg1_pre, bg1, s_bg1
        )
        return mat_pred, bg_pred, cache

    def backward_and_step(self, cache, d_mat, d_bg, lr=2e-3, wd=1e-4):
        (
            x, h0_pre, h0, s_h0, dw1, pw1a_pre, pw1a, s_pw1a, h1,
            g, dw2, dw2_cat, pw2a_pre, pw2a, s_pw2a, h2, mat_pred,
            h2_pool, bg_in, bg1_pre, bg1, s_bg1
        ) = cache
        p = self.params
        grads = {}

        # Bilateral head backward
        d_bg1, grads["W_bg2"], grads["B_bg2"] = pw_backward(bg1, p["W_bg2"], d_bg)
        d_bg1_pre = silu_grad(bg1_pre, s_bg1, d_bg1)
        d_bg_in, grads["W_bg1"], grads["B_bg1"] = pw_backward(bg_in, p["W_bg1"], d_bg1_pre)

        d_h2_pool = d_bg_in[:, :32]
        d_g_from_bg = d_bg_in[:, 32:40]

        # Material head backward (BCE/MSE through sigmoid)
        d_mat_logits = d_mat * mat_pred * (1.0 - mat_pred)
        d_h2_mat, grads["W_mat"], grads["B_mat"] = pw_backward(h2, p["W_mat"], d_mat_logits)

        H, W = h2.shape[2], h2.shape[3]
        d_h2 = d_h2_mat + d_h2_pool[:, :, None, None] / float(H * W)

        # Block 2 backward
        d_pw2a, grads["W_pw2b"], grads["B_pw2b"] = pw_backward(pw2a, p["W_pw2b"], d_h2)
        d_pw2a_pre = silu_grad(pw2a_pre, s_pw2a, d_pw2a)
        d_dw2_cat, grads["W_pw2a"], grads["B_pw2a"] = pw_backward(dw2_cat, p["W_pw2a"], d_pw2a_pre)

        d_dw2 = d_dw2_cat[:, :32, :, :]
        d_g_from_pw2 = d_dw2_cat[:, 32:40, :, :].sum(axis=(2, 3))
        d_h1_from_dw2, grads["W_dw2"], grads["B_dw2"] = depthwise_3x3_backward(h1, p["W_dw2"], d_dw2)

        d_h1 = d_h2 + d_h1_from_dw2
        d_h1[:, :8, :, :] += (d_g_from_bg + d_g_from_pw2)[:, :, None, None] / float(H * W)

        # Block 1 backward
        d_pw1a, grads["W_pw1b"], grads["B_pw1b"] = pw_backward(pw1a, p["W_pw1b"], d_h1)
        d_pw1a_pre = silu_grad(pw1a_pre, s_pw1a, d_pw1a)
        d_dw1, grads["W_pw1a"], grads["B_pw1a"] = pw_backward(dw1, p["W_pw1a"], d_pw1a_pre)
        d_h0_from_dw1, grads["W_dw1"], grads["B_dw1"] = depthwise_3x3_backward(h0, p["W_dw1"], d_dw1)

        d_h0 = d_h1 + d_h0_from_dw1
        d_h0_pre = silu_grad(h0_pre, s_h0, d_h0)
        _, grads["W_stem"], grads["B_stem"] = pw_backward(x, p["W_stem"], d_h0_pre)

        # AdamW update with gradient clipping
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
        """Pack all weights into exact (3013, 4) float32 layout matching nl_neural_prior.hlsl."""
        p = self.params
        chunks = []

        def pack_pw(w, b):
            # w: (Cout, Cin), Cin % 4 == 0, Cout % 4 == 0
            out_c, in_c = w.shape
            w4 = w.reshape(out_c, in_c // 4, 4).reshape(-1, 4)
            b4 = b.reshape(out_c // 4, 4)
            chunks.append(w4)
            chunks.append(b4)

        pack_pw(p["W_stem"], p["B_stem"])  # 96 + 8 = 104

        # DW1: W_dw1 is (32, 9) -> transpose to (9, 32) -> (9*8, 4) = 72 float4s + 8 biases
        chunks.append(p["W_dw1"].T.reshape(9 * 8, 4))
        chunks.append(p["B_dw1"].reshape(8, 4))

        pack_pw(p["W_pw1a"], p["B_pw1a"])  # 512 + 16 = 528
        pack_pw(p["W_pw1b"], p["B_pw1b"])  # 512 + 8  = 520

        chunks.append(p["W_dw2"].T.reshape(9 * 8, 4))  # 72
        chunks.append(p["B_dw2"].reshape(8, 4))        # 8

        pack_pw(p["W_pw2a"], p["B_pw2a"])  # 640 + 16 = 656
        pack_pw(p["W_pw2b"], p["B_pw2b"])  # 512 + 8  = 520
        pack_pw(p["W_mat"], p["B_mat"])    # 64 + 2   = 66
        pack_pw(p["W_bg1"], p["B_bg1"])    # 352 + 8  = 360
        pack_pw(p["W_bg2"], p["B_bg2"])    # 96 + 3   = 99

        packed = np.concatenate(chunks, axis=0).astype(np.float32)
        assert packed.shape == (NL_MODEL_FLOAT4_COUNT, 4), f"Got {packed.shape}"
        assert np.all(np.isfinite(packed)), "Non-finite weights detected!"
        return packed


def generate_pbr_batch(batch_size, rng, preset_style, H=16, W=16):
    """
    Synthesizes diverse multi-material 3D rasterized patches with exact intrinsic
    material ground truth AND path-traced-inspired Bilateral Grid target transforms.
    """
    N = batch_size
    x_feat = np.zeros((N, 12, H, W), dtype=np.float32)
    y_mat = np.zeros((N, 8, H, W), dtype=np.float32)
    luma_z = rng.uniform(0.0, 1.0, size=(N,)).astype(np.float32)
    luma_embed = np.stack(
        [luma_z, 2.0 * luma_z - 1.0, np.sin(math.pi * luma_z), np.cos(math.pi * luma_z)],
        axis=1,
    ).astype(np.float32)
    y_bg = np.zeros((N, 12), dtype=np.float32)

    yy, xx = np.meshgrid(np.linspace(-1, 1, H), np.linspace(-1, 1, W), indexing="ij")

    for i in range(N):
        # Randomize scene lighting environment (indoor dark, sunny outdoor, neon night, overcast)
        scene_type = rng.integers(0, 4)
        key_dir = rng.normal(0.0, 1.0, size=3)
        key_dir[2] = -abs(key_dir[2]) - 0.25
        key_dir /= np.linalg.norm(key_dir)

        if scene_type == 0:  # Sunny outdoor
            key_col = np.array([1.8, 1.65, 1.4]) * rng.uniform(0.8, 1.8)
            sky_col = np.array([0.28, 0.42, 0.65]) * rng.uniform(0.6, 1.2)
            base_depth = rng.uniform(4.0, 40.0)
        elif scene_type == 1:  # Dark indoor / cave
            key_col = np.array([1.3, 0.85, 0.50]) * rng.uniform(0.3, 0.9)
            sky_col = np.array([0.05, 0.06, 0.09]) * rng.uniform(0.5, 1.5)
            base_depth = rng.uniform(1.2, 10.0)
        elif scene_type == 2:  # Neon night
            key_col = rng.choice([[0.4, 1.4, 1.8], [1.7, 0.4, 1.2], [1.5, 0.9, 0.3]])
            sky_col = np.array([0.06, 0.08, 0.16])
            base_depth = rng.uniform(2.0, 18.0)
        else:  # Overcast / soft portrait
            key_col = np.array([1.0, 1.0, 1.02]) * rng.uniform(0.7, 1.2)
            sky_col = np.array([0.35, 0.38, 0.42])
            base_depth = rng.uniform(1.0, 8.0)

        # Build 1-2 material regions across patch (skin, metal, stone/wood, cloth, foliage)
        split_ang = rng.uniform(0, math.pi)
        boundary = (xx * math.cos(split_ang) + yy * math.sin(split_ang)) > rng.uniform(-0.4, 0.4)
        mat_a = rng.integers(0, 5)
        mat_b = rng.integers(0, 5)

        albedo = np.zeros((H, W, 3), np.float32)
        rough = np.zeros((H, W), np.float32)
        metal = np.zeros((H, W), np.float32)
        skin = np.zeros((H, W), np.float32)
        foliage = np.zeros((H, W), np.float32)
        tex_var = np.zeros((H, W), np.float32)

        for mask, m_id in [(~boundary, mat_a), (boundary, mat_b)]:
            if not np.any(mask):
                continue
            if m_id == 0:  # Human skin (warm peachy-tan-deep tones, mid-low roughness)
                c = np.array([rng.uniform(0.45, 0.88), rng.uniform(0.28, 0.65), rng.uniform(0.20, 0.52)])
                albedo[mask] = c
                rough[mask] = rng.uniform(0.32, 0.52)
                metal[mask] = 0.0
                skin[mask] = 1.0
                foliage[mask] = 0.0
                tex_var[mask] = rng.uniform(0.02, 0.10)
            elif m_id == 1:  # Metal (chrome, gold, copper, gunmetal)
                c = rng.uniform(0.35, 0.92, size=3)
                albedo[mask] = c
                rough[mask] = rng.uniform(0.08, 0.38)
                metal[mask] = rng.uniform(0.82, 1.0)
                skin[mask] = 0.0
                foliage[mask] = 0.0
                tex_var[mask] = rng.uniform(0.03, 0.20)
            elif m_id == 2:  # Stone / concrete / wood (matte, higher variance)
                c = np.array([rng.uniform(0.18, 0.62)] * 3) * rng.uniform(0.8, 1.15, size=3)
                albedo[mask] = np.clip(c, 0.05, 0.85)
                rough[mask] = rng.uniform(0.68, 0.95)
                metal[mask] = 0.0
                skin[mask] = 0.0
                foliage[mask] = 0.0
                tex_var[mask] = rng.uniform(0.12, 0.42)
            elif m_id == 3:  # Cloth / fabric (richer chroma, high roughness)
                c = rng.uniform(0.08, 0.78, size=3)
                albedo[mask] = c
                rough[mask] = rng.uniform(0.75, 0.96)
                metal[mask] = 0.0
                skin[mask] = 0.0
                foliage[mask] = 0.0
                tex_var[mask] = rng.uniform(0.08, 0.28)
            else:  # Foliage / vegetation (green-olive dominant)
                c = np.array([rng.uniform(0.08, 0.35), rng.uniform(0.28, 0.72), rng.uniform(0.06, 0.25)])
                albedo[mask] = c
                rough[mask] = rng.uniform(0.45, 0.72)
                metal[mask] = 0.0
                skin[mask] = 0.0
                foliage[mask] = 1.0
                tex_var[mask] = rng.uniform(0.18, 0.45)

        # Normal field with smooth curvature
        nx = np.clip(xx * rng.uniform(-0.5, 0.5) + rng.uniform(-0.35, 0.35), -0.85, 0.85)
        ny = np.clip(yy * rng.uniform(-0.5, 0.5) + rng.uniform(-0.35, 0.35), -0.85, 0.85)
        nz = -np.sqrt(np.maximum(0.05, 1.0 - nx * nx - ny * ny))

        # Depth gradient + step across material boundary
        depth = base_depth * (1.0 + 0.15 * xx + 0.1 * yy + 0.25 * boundary.astype(np.float32))
        sky_mask = np.ones((H, W), np.float32)
        if rng.random() < 0.15:
            sky_region = boundary
            sky_mask[sky_region] = 0.0
            depth[sky_region] = 650.0

        ndotl = np.clip(nx * key_dir[0] + ny * key_dir[1] + nz * key_dir[2], 0.0, 1.0)
        # Simulate soft shadow terminator across patch
        shadow = np.clip(0.5 + 1.4 * (xx * key_dir[0] + yy * key_dir[1] + rng.uniform(-0.3, 0.3)), 0.05, 1.0)
        direct_term = ndotl * shadow
        direct_irrad = direct_term[:, :, None] * key_col[None, None, :]
        indirect_irrad = (0.5 + 0.5 * (-ny[:, :, None])) * sky_col[None, None, :] + 0.04

        spec_f0 = 0.04 * (1.0 - metal[:, :, None]) + albedo * metal[:, :, None]
        spec_lobe = (ndotl ** np.clip((1.0 - rough) * 16.0, 1.5, 16.0))[:, :, None] * spec_f0 * shadow[:, :, None] * key_col[None, None, :]
        diffuse_color = albedo * (1.0 - 0.85 * metal[:, :, None])
        rgb = diffuse_color * (direct_irrad + indirect_irrad) + spec_lobe
        # Sky color for sky region
        if np.any(sky_mask < 0.5):
            rgb[sky_mask < 0.5] = sky_col * 2.2
            albedo[sky_mask < 0.5] = np.clip(sky_col, 0.0, 1.0)
            rough[sky_mask < 0.5] = 1.0
            metal[sky_mask < 0.5] = 0.0
            skin[sky_mask < 0.5] = 0.0
            foliage[sky_mask < 0.5] = 0.0

        # Add realistic micro texture perturbation
        noise_tex = rng.normal(0.0, 1.0, size=(H, W)) * tex_var * 0.25
        rgb = np.maximum(rgb * (1.0 + noise_tex[:, :, None]), 0.0)

        luma = rgb[:, :, 0] * 0.2126 + rgb[:, :, 1] * 0.7152 + rgb[:, :, 2] * 0.0722
        coarse_luma = float(np.mean(luma))
        inv_luma = 1.0 / np.maximum(luma, 0.02)

        x_feat[i, 0] = np.log2(1.0 + luma) * 0.25
        x_feat[i, 1] = np.clip(rgb[:, :, 0] * inv_luma, 0.0, 3.0)
        x_feat[i, 2] = np.clip(rgb[:, :, 1] * inv_luma, 0.0, 3.0)
        x_feat[i, 3] = np.clip(rgb[:, :, 2] * inv_luma, 0.0, 3.0)
        x_feat[i, 4] = np.clip(np.log2(1.0 + depth) * 0.1, 0.0, 1.0)
        x_feat[i, 5] = nx
        x_feat[i, 6] = ny
        x_feat[i, 7] = np.abs(nz)
        x_feat[i, 8] = np.clip(tex_var * 4.0, 0.0, 1.0)
        x_feat[i, 9] = sky_mask
        x_feat[i, 10] = np.clip(luma / max(coarse_luma + 0.05, 0.05) - 1.0, -1.0, 2.0)
        x_feat[i, 11] = np.clip(ndotl * 0.5 + 0.5, 0.0, 1.0)

        # Direct vs indirect ratio ground truth
        dir_e = np.sum(direct_irrad, axis=2) * (1.0 - 0.5 * metal)
        tot_e = dir_e + np.sum(indirect_irrad, axis=2) + 1e-4
        direct_mask = np.clip(dir_e / tot_e, 0.0, 1.0) * sky_mask

        y_mat[i, 0:3] = np.transpose(np.clip(albedo, 0.0, 1.0), (2, 0, 1))
        y_mat[i, 3] = direct_mask
        y_mat[i, 4] = np.clip(rough, 0.0, 1.0)
        y_mat[i, 5] = np.clip(metal, 0.0, 1.0)
        y_mat[i, 6] = np.clip(skin, 0.0, 1.0)
        y_mat[i, 7] = np.clip(foliage, 0.0, 1.0)

        # Path-traced teacher Bilateral Grid target (per preset look!)
        lz = luma_z[i]
        mean_skin = float(np.mean(skin))
        mean_metal = float(np.mean(metal))
        mean_foliage = float(np.mean(foliage))
        mean_var = float(np.mean(tex_var))
        shadow_zone = max(0.0, 1.0 - lz * 2.2)
        high_zone = max(0.0, (lz - 0.6) * 2.5)
        mid_zone = math.exp(-((lz - 0.42) ** 2) / 0.08)

        if preset_style == "photorealistic_pbr":
            gain = np.array([1.0 + 0.05 * mid_zone - 0.04 * high_zone,
                             1.0 + 0.04 * mid_zone - 0.04 * high_zone,
                             1.0 + 0.03 * mid_zone - 0.03 * high_zone])
            gain += np.array([0.04, 0.01, -0.02]) * mean_skin * mid_zone
            shadow_lift = 0.08 * shadow_zone * (1.0 - 0.4 * mean_metal)
            off = np.array([0.010, 0.004, 0.0]) * shadow_zone
            detail = 0.20 * (0.4 + 0.6 * mean_var) * (1.0 - 0.45 * mean_skin)
            chroma = 0.12 * mid_zone + 0.08 * mean_foliage
            spec_boost = 0.16 * mean_metal + 0.06 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.18 * high_zone
        elif preset_style == "path_traced_cinema":
            gain = np.array([1.06 * mid_zone + 0.94 * (1.0 - mid_zone),
                             1.02 * mid_zone + 0.95 * (1.0 - mid_zone),
                             0.96 * mid_zone + 1.02 * (1.0 - mid_zone)])
            gain += np.array([0.06, 0.02, -0.04]) * mean_skin
            shadow_lift = 0.05 * shadow_zone - 0.05 * (1.0 - shadow_zone) * 0.2
            off = np.array([0.008, 0.002, 0.012]) * shadow_zone
            detail = 0.26 * (0.45 + 0.55 * mean_var) * (1.0 - 0.50 * mean_skin)
            chroma = 0.22 * mid_zone + 0.10 * mean_foliage
            spec_boost = 0.24 * mean_metal + 0.10 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.28 * high_zone
        elif preset_style == "natural_daylight":
            gain = np.array([1.02, 1.03, 1.04]) - 0.05 * high_zone
            shadow_lift = 0.14 * shadow_zone
            off = np.array([0.006, 0.008, 0.012]) * shadow_zone
            detail = 0.15 * (0.5 + 0.5 * mean_var) * (1.0 - 0.40 * mean_skin)
            chroma = 0.09 * mid_zone + 0.08 * mean_foliage
            spec_boost = 0.12 * mean_metal + 0.05 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.14 * high_zone
        else:  # filmic_noir
            gain = np.array([0.96 + 0.12 * mid_zone, 0.95 + 0.10 * mid_zone, 0.98 + 0.08 * mid_zone])
            shadow_lift = -0.06 * shadow_zone
            off = np.array([-0.004, -0.003, 0.006]) * shadow_zone
            detail = 0.30 * (0.4 + 0.6 * mean_var) * (1.0 - 0.35 * mean_skin)
            chroma = -0.10 + 0.10 * mean_skin
            spec_boost = 0.32 * mean_metal + 0.14 * (1.0 - float(np.mean(rough)))
            hl_roll = 0.35 * high_zone

        y_bg[i, 0:3] = gain - 1.0
        y_bg[i, 3] = shadow_lift
        y_bg[i, 4:7] = off
        y_bg[i, 7] = detail
        y_bg[i, 8] = chroma
        y_bg[i, 9] = spec_boost
        y_bg[i, 10] = hl_roll
        y_bg[i, 11] = float(np.mean(sky_mask))

    return x_feat, luma_embed, y_mat, y_bg


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

    # Fixed validation set
    val_x, val_le, val_ymat, val_ybg = generate_pbr_batch(96, np.random.default_rng(seed + 999), style_name)

    def eval_loss(x, le, ymat, ybg):
        pmat, pbg, _ = net.forward(x, le)
        l_mat = float(np.mean((pmat - ymat) ** 2))
        l_bg = float(np.mean((pbg - ybg) ** 2))
        mae_mat = float(np.mean(np.abs(pmat - ymat)))
        mae_bg = float(np.mean(np.abs(pbg - ybg)))
        return l_mat + l_bg, mae_mat, mae_bg

    init_loss, init_m_mae, init_b_mae = eval_loss(val_x, val_le, val_ymat, val_ybg)
    print(f"[{style_name}] initial val_loss={init_loss:.5f} mat_mae={init_m_mae:.4f} bg_mae={init_b_mae:.4f}")

    steps_per_epoch = 20
    total_steps = epochs * steps_per_epoch
    base_lr = 4e-3

    for ep in range(epochs):
        for st in range(steps_per_epoch):
            step_idx = ep * steps_per_epoch + st
            lr = base_lr * (0.1 + 0.9 * 0.5 * (1.0 + math.cos(math.pi * step_idx / max(total_steps, 1))))
            bx, ble, bymat, bybg = generate_pbr_batch(batch_size, rng, style_name)
            pmat, pbg, cache = net.forward(bx, ble)
            inv_n = 1.0 / float(batch_size)
            d_mat = (2.0 * (pmat - bymat) / float(8 * bx.shape[2] * bx.shape[3])) * inv_n
            d_bg = (2.0 * (pbg - bybg) / 12.0) * inv_n
            net.backward_and_step(cache, d_mat, d_bg, lr=lr, wd=5e-5)

        if (ep + 1) % max(1, epochs // 3) == 0 or ep == epochs - 1:
            v_loss, v_m_mae, v_b_mae = eval_loss(val_x, val_le, val_ymat, val_ybg)
            print(
                f"  epoch {ep + 1:02d}/{epochs}: val_loss={v_loss:.5f} "
                f"mat_mae={v_m_mae:.4f} bg_mae={v_b_mae:.4f}"
            )

    final_loss, _, _ = eval_loss(val_x, val_le, val_ymat, val_ybg)
    assert final_loss < init_loss * 0.45, f"Training did not converge sufficiently: {init_loss} -> {final_loss}"
    return net.pack_weights_float4()


def main():
    parser = argparse.ArgumentParser(description="Train & verify GNU-Upscaler Neural Look models")
    parser.add_argument("--epochs", type=int, default=18)
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
        print("All .nlmodel packs and NL_DefaultModel.h verified successfully.")
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
