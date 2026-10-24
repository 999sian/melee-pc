#pragma once
#include <string_view>

namespace aurora::gx {
inline constexpr std::string_view UberShaderFunctions = R"wgsl(
struct UberStage {
    color_args: vec4u,
    alpha_args: vec4u,
    color_op: vec4u,
    alpha_op: vec4u,
    konst: vec4u,
};
fn uber_kcolor(sel: u32, colors: array<vec4f, 4>) -> vec3f {
    if (sel < 8u) { return vec3f(f32(8u - sel) / 8.0); }
    if (sel < 16u) { return colors[sel - 12u].rgb; }
    return vec3f(colors[(sel - 16u) % 4u][(sel - 16u) / 4u]);
}
fn uber_kalpha(sel: u32, colors: array<vec4f, 4>) -> f32 {
    if (sel < 8u) { return f32(8u - sel) / 8.0; }
    return colors[(sel - 16u) % 4u][(sel - 16u) / 4u];
}
fn uber_color_arg(arg: u32, regs: array<vec4f, 4>, tex: vec4f, ras: vec4f,
                  konst: vec3f, wrap: bool) -> vec3f {
    if (arg < 8u) {
        let reg = regs[arg / 2u];
        let value = select(reg.rgb, vec3f(reg.a), (arg & 1u) != 0u);
        if (wrap) { return tev_overflow_vec3f(value); }
        return value;
    }
    switch arg {
        case 8u: { return tex.rgb; }
        case 9u: { return vec3f(tex.a); }
        case 10u: { return ras.rgb; }
        case 11u: { return vec3f(ras.a); }
        case 12u: { return vec3f(1.0); }
        case 13u: { return vec3f(0.5); }
        case 14u: { return konst; }
        default: { return vec3f(0.0); }
    }
}
fn uber_alpha_arg(arg: u32, regs: array<vec4f, 4>, tex: vec4f, ras: vec4f,
                  konst: f32, wrap: bool) -> f32 {
    if (arg < 4u) {
        let value = regs[arg].a;
        if (wrap) { return tev_overflow_f32(value); }
        return value;
    }
    switch arg {
        case 4u: { return tex.a; }
        case 5u: { return ras.a; }
        case 6u: { return konst; }
        default: { return 0.0; }
    }
}
fn uber_combine(a: vec4f, b: vec4f, c: vec4f, d: vec4f, op: vec4u) -> vec4f {
    let bias = select(select(0.0, 0.5, op.y == 1u), -0.5, op.y == 2u);
    let scale = array<f32, 4>(1.0, 2.0, 4.0, 0.5)[op.z];
    let value = (select(1.0, -1.0, op.x == 1u) * mix(a, b, c) + d + vec4f(bias)) * scale;
    if ((op.w & 256u) != 0u) { return clamp(value, vec4f(0.0), vec4f(1.0)); }
    return clamp(value, vec4f(-4.0), vec4f(4.0));
}
fn uber_compare(alpha: u32, op: u32, reference: u32) -> bool {
    switch op {
        case 0u: { return false; }
        case 1u: { return alpha < reference; }
        case 2u: { return alpha == reference; }
        case 3u: { return alpha <= reference; }
        case 4u: { return alpha > reference; }
        case 5u: { return alpha != reference; }
        case 6u: { return alpha >= reference; }
        default: { return true; }
    }
}
fn uber_alpha_test(alpha: f32, test0: vec4u, test1: vec4u) -> bool {
    let byte_alpha = u32(round(clamp(alpha, 0.0, 1.0) * 255.0));
    let a = uber_compare(byte_alpha, test0.x, test0.y);
    let b = uber_compare(byte_alpha, test0.w, test1.x);
    switch test0.z {
        case 0u: { return a && b; }
        case 1u: { return a || b; }
        case 2u: { return a != b; }
        default: { return a == b; }
    }
}
)wgsl";
} // namespace aurora::gx
