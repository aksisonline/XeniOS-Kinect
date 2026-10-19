// Native Kinect pose tracker for the Xenia skeleton hook (Swift + Apple Vision).
//
// Reads the depth + Bayer colour frames Xenia (or harness/kinect_publish) writes to
// /tmp/xenia_kinect_frames, runs VNDetectHumanBodyPose3DRequest (Neural Engine) on the colour
// image, places the body with the Kinect's depth, maps it to the Kinect v1 20-joint skeleton,
// filters it, and (with --send) sends it to Xenia over UDP. A viewer window is always shown.
//
// Packet (little endian, 272 bytes): 'KSKL', u32 seq, u32 tracked, float32 joints[20][3]
// (metres, sensor space: +y up, +z forward, +x to the sensor's LEFT), u8 joint_state[20].
//
// build: ./build.sh     run: ./kinect_tracker_native [--send] [--z-offset 0.8] [--port 5760]

import AppKit
import CoreVideo
import Foundation
import SceneKit
import Vision
import simd

// MARK: - constants

let W = 640, H = 480
let FX_C: Float = 529.2, FX_D: Float = 594.2, CX: Float = 320, CY: Float = 240
let D_SCALE = FX_D / FX_C
let depthOffset = 0x40, colorOffset = 0x40 + W * H * 2

// Kinect joint indices
enum K {
    static let hipC = 0, spine = 1, shoulderC = 2, head = 3
    static let shL = 4, elL = 5, wrL = 6, hdL = 7, shR = 8, elR = 9, wrR = 10, hdR = 11
    static let hipL = 12, knL = 13, anL = 14, ftL = 15, hipR = 16, knR = 17, anR = 18, ftR = 19
}
let bones: [(Int, Int)] = [(0, 1), (1, 2), (2, 3), (2, 4), (4, 5), (5, 6), (6, 7), (2, 8), (8, 9), (9, 10),
                           (10, 11), (0, 12), (12, 13), (13, 14), (14, 15), (0, 16), (16, 17), (17, 18), (18, 19)]
let boneChains: [(Int, Int)] = [(0, 1), (1, 2), (2, 3), (4, 5), (5, 6), (6, 7), (8, 9), (9, 10), (10, 11),
                                (12, 13), (13, 14), (14, 15), (16, 17), (17, 18), (18, 19)]

struct Options {
    var send = false
    var port: UInt16 = 5760
    var zOffset: Float = 0.8
    var bodyOffset: Float = 0.08
    var minCutoff: Float = 1.0
    var beta: Float = 6.0
    var zCutoff: Float = 0.3
    var handLength: Float = 0.09
    var depthBlend: Float = 0.5
    var boneLock = true
    var grace = 20
}
var opts = Options()
// Live-tunable from the viewer (keys): base cutoff (Hz) at rest, speed coefficient, depth cutoff (Hz).
var tuneCutoff: Float = 3.0, tuneBeta: Float = 12.0, tuneZCutoff: Float = 0.8
var sendEnabled = false
do {
    var it = CommandLine.arguments.dropFirst().makeIterator()
    while let a = it.next() {
        switch a {
        case "--send": opts.send = true; sendEnabled = true
        case "--port": opts.port = UInt16(it.next() ?? "") ?? 5760
        case "--z-offset": opts.zOffset = Float(it.next() ?? "") ?? 0.8
        case "--body-offset": opts.bodyOffset = Float(it.next() ?? "") ?? 0.08
        case "--min-cutoff": tuneCutoff = Float(it.next() ?? "") ?? 3.0
        case "--beta": tuneBeta = Float(it.next() ?? "") ?? 12.0
        case "--z-cutoff": tuneZCutoff = Float(it.next() ?? "") ?? 0.8
        case "--depth-blend": opts.depthBlend = Float(it.next() ?? "") ?? 0.5
        case "--no-bone-lock": opts.boneLock = false
        case "--grace": opts.grace = Int(it.next() ?? "") ?? 20
        default: FileHandle.standardError.write("unknown option \(a)\n".data(using: .utf8)!)
        }
    }
}

// MARK: - shared frames

final class Frames {
    let base: UnsafeMutableRawPointer
    init?() {
        let fd = open("/tmp/xenia_kinect_frames", O_RDONLY)
        guard fd >= 0 else { return nil }
        let size = colorOffset + W * H
        guard let p = mmap(nil, size, PROT_READ, MAP_SHARED, fd, 0), p != MAP_FAILED else { return nil }
        close(fd)
        base = p
    }
    var colorSeq: UInt32 { base.load(fromByteOffset: 8, as: UInt32.self) }
    var depth: UnsafePointer<UInt16> { UnsafePointer(base.advanced(by: depthOffset).assumingMemoryBound(to: UInt16.self)) }
    var bayer: UnsafePointer<UInt8> { UnsafePointer(base.advanced(by: colorOffset).assumingMemoryBound(to: UInt8.self)) }
}

// raw 11-bit disparity -> metres (Magnenat's fit), 0 = invalid
let rawToMetres: [Float] = (0..<2048).map { r in
    (r > 0 && r < 1085) ? 1.0 / (Float(r) * -0.0030711016 + 3.3309495161) : 0
}

// MARK: - helpers

func medianOf(_ v: [Float]) -> Float {
    if v.isEmpty { return 0 }
    let s = v.sorted()
    return s[s.count / 2]
}

/// Median valid depth (m) in a colour-pixel box, mapped to depth pixels.
func depthBox(_ depth: UnsafePointer<UInt16>, u0: Float, u1: Float, v0: Float, v1: Float) -> (median: Float, count: Int) {
    let du0 = Int(CX + (u0 - CX) * D_SCALE), du1 = Int(CX + (u1 - CX) * D_SCALE)
    let dv0 = Int(CY + (v0 - CY) * D_SCALE), dv1 = Int(CY + (v1 - CY) * D_SCALE)
    var values: [Float] = []
    for y in max(dv0, 0)...max(min(dv1, H - 1), max(dv0, 0)) {
        for x in max(du0, 0)...max(min(du1, W - 1), max(du0, 0)) {
            let m = rawToMetres[Int(min(depth[y * W + x], 2047))]
            if m > 0 { values.append(m) }
        }
    }
    return (medianOf(values), values.count)
}

struct OneEuro {
    var minCutoff: SIMD3<Float>, beta: SIMD3<Float>, dCutoff: Float = 1.0
    var prev: SIMD3<Float>? = nil, dx = SIMD3<Float>(repeating: 0), t: Double = 0
    static func alpha(_ cutoff: SIMD3<Float>, _ dt: Float) -> SIMD3<Float> {
        let tau = 1.0 / (2 * Float.pi * cutoff)
        return 1.0 / (1.0 + tau / dt)
    }
    mutating func reset() { prev = nil }
    mutating func filter(_ x: SIMD3<Float>, _ now: Double) -> SIMD3<Float> {
        guard let p = prev else { prev = x; dx = .zero; t = now; return x }
        let dt = Float(max(now - t, 1e-3))
        let ad = OneEuro.alpha(SIMD3<Float>(repeating: dCutoff), dt)
        let dxHat = ad * (x - p) / dt + (SIMD3<Float>(repeating: 1) - ad) * dx
        let a = OneEuro.alpha(minCutoff + beta * simd_abs(dxHat), dt)
        let xHat = a * x + (SIMD3<Float>(repeating: 1) - a) * p
        prev = xHat; dx = dxHat; t = now
        return xHat
    }
}

// MARK: - tracker state shared with the viewer

struct Snapshot {
    var image: Data? = nil          // BGRA 640x480
    var depthImage: Data? = nil     // BGRA 640x480 colour-mapped depth
    var torsoBox: CGRect? = nil     // colour-pixel box the torso depth was sampled from
    var raw = [SIMD3<Float>](repeating: .zero, count: 20)
    var filtered = [SIMD3<Float>](repeating: .zero, count: 20)
    var states = [UInt8](repeating: 0, count: 20)
    var tracked = false
    var fps: Float = 0, pipelineMs: Float = 0, visionMs: Float = 0, trackedPercent: Float = 0
    var sent: UInt32 = 0
    var visionJoints2D: [CGPoint] = []   // image coordinates, top-left origin
    var note = ""
    var headZRaw: [Float] = [], headZFiltered: [Float] = []
}
let snapshotLock = NSLock()
var snapshot = Snapshot()

// MARK: - UDP

final class Sender {
    let fd = socket(AF_INET, SOCK_DGRAM, 0)
    var addr = sockaddr_in()
    var seq: UInt32 = 0
    init(port: UInt16) {
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        addr.sin_port = port.bigEndian
    }
    func send(joints: [SIMD3<Float>], states: [UInt8], tracked: Bool) {
        seq &+= 1
        var packet = Data()
        packet.append(contentsOf: Array("KSKL".utf8))
        var s = seq.littleEndian, t = UInt32(tracked ? 1 : 0).littleEndian
        withUnsafeBytes(of: &s) { packet.append(contentsOf: $0) }
        withUnsafeBytes(of: &t) { packet.append(contentsOf: $0) }
        for j in joints {
            for c in [j.x, j.y, j.z] {
                var f = c.bitPattern.littleEndian
                withUnsafeBytes(of: &f) { packet.append(contentsOf: $0) }
            }
        }
        packet.append(contentsOf: states)
        packet.withUnsafeBytes { raw in
            withUnsafePointer(to: &addr) { a in
                a.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    _ = sendto(fd, raw.baseAddress, raw.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }
}

// MARK: - pipeline

final class Pipeline {
    let frames: Frames
    var pixelBuffer: CVPixelBuffer!
    let sequence = VNSequenceRequestHandler()
    let request3D = VNDetectHumanBodyPose3DRequest()
    let request2D = VNDetectHumanBodyPoseRequest()
    var filters: [OneEuro] = []
    var cutFactor: [Float] = [], betaFactor: [Float] = []
    var zFilter = OneEuro(minCutoff: SIMD3(repeating: 0.8), beta: SIMD3(repeating: 2.0))
    var depthHistory: [Float] = []
    var lengths: [Int: Float] = [:]
    var boneCount = 0
    var lostFrames = 0
    var lastJoints: [SIMD3<Float>]? = nil, lastStates: [UInt8] = []
    let sender = Sender(port: opts.port)
    var trackedFrames = 0, totalFrames = 0
    var headRaw: [Float] = [], headFiltered: [Float] = []

    init(frames: Frames) {
        self.frames = frames
        CVPixelBufferCreate(kCFAllocatorDefault, W, H, kCVPixelFormatType_32BGRA, nil, &pixelBuffer)
        // Per joint factors on the live cutoff/beta: elbows a little smoother, wrists/hands a little
        // smoother at rest but with a higher speed term so fast motion does not lag.
        for j in 0..<20 {
            var cutoff: Float = 1.0, beta: Float = 1.0
            if j == K.elL || j == K.elR { cutoff = 0.9; beta = 1.2 }
            if [K.wrL, K.hdL, K.wrR, K.hdR].contains(j) { cutoff = 0.8; beta = 1.6 }
            cutFactor.append(cutoff); betaFactor.append(beta)
            filters.append(OneEuro(minCutoff: SIMD3(repeating: 1), beta: SIMD3(repeating: 1)))
        }
    }

    func applyTuning() {
        for j in 0..<20 {
            filters[j].minCutoff = tuneCutoff * cutFactor[j] * SIMD3<Float>(1, 1, 0.5)
            filters[j].beta = tuneBeta * betaFactor[j] * SIMD3<Float>(1, 1, 0.25)
        }
        zFilter.minCutoff = SIMD3(repeating: tuneZCutoff)
    }

    var torsoBox: CGRect? = nil
    // jet-like colour map: near = red, far = blue; black = no reading
    static let palette: [UInt32] = (0..<256).map { i in
        let t = Float(i) / 255
        func ch(_ x: Float) -> UInt32 { UInt32(255 * min(max(1.5 - abs(4 * x - 3.0), 0), 1)) }
        let r = ch(1 - t + 0.0 + 0.0 + 0.25 - 0.25), g = ch(0.0 + (1 - abs(2 * t - 1)) * 1.5 + 0.5 - 0.5 + 0), b = ch(t + 0.25 - 0.25)
        _ = (r, g, b)
        // simple hue ramp: red (near) -> yellow -> green -> cyan -> blue (far)
        let h = (1 - t) * 0.66 * 6
        let x = UInt32(255 * (1 - abs(h.truncatingRemainder(dividingBy: 2) - 1)))
        let (rr, gg, bb): (UInt32, UInt32, UInt32)
        switch Int(h) {
        case 0: (rr, gg, bb) = (255, x, 0)
        case 1: (rr, gg, bb) = (x, 255, 0)
        case 2: (rr, gg, bb) = (0, 255, x)
        default: (rr, gg, bb) = (0, x, 255)
        }
        return 0xFF000000 | (rr << 16) | (gg << 8) | bb
    }

    func makeDepthImage() -> Data {
        var data = Data(count: W * H * 4)
        data.withUnsafeMutableBytes { raw in
            let out = raw.bindMemory(to: UInt32.self)
            let depth = frames.depth
            for i in 0..<(W * H) {
                let m = rawToMetres[Int(min(depth[i], 2047))]
                out[i] = m == 0 ? 0xFF000000 : Pipeline.palette[Int(min(max((m - 0.5) / 3.5, 0), 1) * 255)]
            }
        }
        return data
    }

    /// GRBG Bayer -> BGRA (2x2 block replication).
    func demosaic() {
        CVPixelBufferLockBaseAddress(pixelBuffer, [])
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }
        let dst = CVPixelBufferGetBaseAddress(pixelBuffer)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let src = frames.bayer
        for by in 0..<(H / 2) {
            for bx in 0..<(W / 2) {
                let i = (2 * by) * W + 2 * bx
                let g0 = Int(src[i]), r = src[i + 1], b = src[i + W], g1 = Int(src[i + W + 1])
                let g = UInt8((g0 + g1) / 2)
                for dy in 0..<2 {
                    for dx in 0..<2 {
                        let o = (2 * by + dy) * stride + (2 * bx + dx) * 4
                        dst[o] = b; dst[o + 1] = g; dst[o + 2] = r; dst[o + 3] = 255
                    }
                }
            }
        }
    }

    func run() {
        var lastSeq: UInt32 = 0
        var windowStart = CFAbsoluteTimeGetCurrent(), windowFrames = 0
        var pipeAccum: Double = 0, visionAccum: Double = 0
        while true {
            let seq = frames.colorSeq
            if seq == lastSeq { usleep(2000); continue }
            lastSeq = seq
            let tStart = CFAbsoluteTimeGetCurrent()
            applyTuning()
            demosaic()

            let tVision = CFAbsoluteTimeGetCurrent()
            try? sequence.perform([request3D, request2D], on: pixelBuffer, orientation: .up)
            visionAccum += CFAbsoluteTimeGetCurrent() - tVision

            var raw = [SIMD3<Float>](repeating: .zero, count: 20)
            var states = [UInt8](repeating: 0, count: 20)
            var points2D: [CGPoint] = []
            var tracked = buildSkeleton(raw: &raw, states: &states, points2D: &points2D)

            var filtered = raw
            let now = CFAbsoluteTimeGetCurrent()
            if tracked {
                if opts.boneLock { raw = lockBones(raw, states) }
                filtered = raw
                for j in 0..<20 { filtered[j] = filters[j].filter(raw[j], now) }
                lastJoints = filtered; lastStates = states; lostFrames = 0
            } else if let last = lastJoints, lostFrames < opts.grace {
                lostFrames += 1   // brief loss: hold the last pose, marked inferred
                tracked = true; filtered = last; raw = last; states = lastStates.map { min($0, 1) }
            } else {
                for j in 0..<20 { filters[j].reset() }
                zFilter.reset(); depthHistory = []; boneCount = 0; lastJoints = nil
            }
            totalFrames += 1; if tracked { trackedFrames += 1 }

            var out = filtered
            if tracked { for j in 0..<20 { out[j].z += opts.zOffset } }
            if sendEnabled { sender.send(joints: out, states: states, tracked: tracked) }

            if tracked {
                headRaw.append(raw[K.head].z); headFiltered.append(filtered[K.head].z)
                if headRaw.count > 180 { headRaw.removeFirst(); headFiltered.removeFirst() }
            }
            let tEnd = CFAbsoluteTimeGetCurrent()
            pipeAccum += tEnd - tStart; windowFrames += 1

            // viewer snapshot
            var image: Data? = nil, depthImage: Data? = nil
            if totalFrames % 2 == 0 {   // viewer redraws at ~15 fps; skip the copy otherwise
                CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
                image = Data(bytes: CVPixelBufferGetBaseAddress(pixelBuffer)!, count: CVPixelBufferGetBytesPerRow(pixelBuffer) * H)
                CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly)
                depthImage = makeDepthImage()
            }
            snapshotLock.lock()
            if let image { snapshot.image = image }
            if let depthImage { snapshot.depthImage = depthImage }
            snapshot.torsoBox = tracked ? torsoBox : nil
            snapshot.raw = raw; snapshot.filtered = filtered; snapshot.states = states
            snapshot.tracked = tracked; snapshot.visionJoints2D = points2D
            snapshot.trackedPercent = 100 * Float(trackedFrames) / Float(max(totalFrames, 1))
            snapshot.headZRaw = headRaw; snapshot.headZFiltered = headFiltered
            snapshot.sent = sender.seq
            if tEnd - windowStart > 1.0 {
                snapshot.fps = Float(Double(windowFrames) / (tEnd - windowStart))
                snapshot.pipelineMs = Float(1000 * pipeAccum / Double(max(windowFrames, 1)))
                snapshot.visionMs = Float(1000 * visionAccum / Double(max(windowFrames, 1)))
                windowStart = tEnd; windowFrames = 0; pipeAccum = 0; visionAccum = 0
            }
            snapshotLock.unlock()
        }
    }

    // MARK: skeleton construction

    func buildSkeleton(raw: inout [SIMD3<Float>], states: inout [UInt8], points2D: inout [CGPoint]) -> Bool {
        guard let obs = request3D.results?.first else { return false }
        let names: [(Int, VNHumanBodyPose3DObservation.JointName)] = [
            (K.hipC, .root), (K.spine, .spine), (K.shoulderC, .centerShoulder), (K.head, .centerHead),
            (K.shL, .leftShoulder), (K.elL, .leftElbow), (K.wrL, .leftWrist),
            (K.shR, .rightShoulder), (K.elR, .rightElbow), (K.wrR, .rightWrist),
            (K.hipL, .leftHip), (K.knL, .leftKnee), (K.anL, .leftAnkle),
            (K.hipR, .rightHip), (K.knR, .rightKnee), (K.anR, .rightAnkle)]
        var cam = [Int: SIMD3<Float>](), pix = [Int: SIMD2<Float>]()
        for (k, name) in names {
            guard let m = try? obs.cameraRelativePosition(name) else { return false }
            cam[k] = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
            if let p = try? obs.pointInImage(name) {
                pix[k] = SIMD2<Float>(Float(p.location.x) * Float(W), (1 - Float(p.location.y)) * Float(H))
                points2D.append(CGPoint(x: Double(pix[k]!.x), y: Double(pix[k]!.y)))
            }
        }
        guard let root = cam[K.hipC], let rootPix = pix[K.hipC] else { return false }

        // Torso depth from the Kinect (median of the torso box, then of the last frames).
        let torsoPix = [K.shL, K.shR, K.hipL, K.hipR].compactMap { pix[$0] }
        guard torsoPix.count == 4 else { return false }
        let us = torsoPix.map { $0.x }, vs = torsoPix.map { $0.y }
        let cu = (us.min()! + us.max()!) / 2, cv = (vs.min()! + vs.max()!) / 2
        let hu = (us.max()! - us.min()!) / 4, hv = (vs.max()! - vs.min()!) / 4
        let box = depthBox(frames.depth, u0: cu - hu, u1: cu + hu, v0: cv - hv, v1: cv + hv)
        torsoBox = CGRect(x: Double(cu - hu), y: Double(cv - hv), width: Double(2 * hu), height: Double(2 * hv))
        var z = depthHistory.last ?? 0
        if box.count > 20 && (depthHistory.isEmpty || abs(box.median - z) < 0.5) {
            depthHistory.append(box.median)
            if depthHistory.count > 7 { depthHistory.removeFirst() }
            z = medianOf(depthHistory)
        }
        guard z > 0 else { return false }
        z = zFilter.filter(SIMD3<Float>(z, z, z), CFAbsoluteTimeGetCurrent()).x
        guard z > 0.8 && z < 4.5 else { return false }
        z += opts.bodyOffset

        // Vision camera space: x right, y up, z along the view axis (sign auto-detected so that
        // moving away from the camera increases depth).
        let zSign: Float = root.z < 0 ? -1 : 1
        let hipK = SIMD3<Float>((CX - rootPix.x) * z / FX_C, (CY - rootPix.y) * z / FX_C, z)
        func toK(_ j: Int) -> SIMD3<Float> {
            let d = cam[j]! - root
            return hipK + SIMD3<Float>(-d.x, d.y, zSign * d.z)
        }
        for (k, _) in names { raw[k] = toK(k) }

        // Hands: wrist extended along the forearm. Feet: ankle plus a small offset toward the sensor.
        for (el, wr, hd) in [(K.elL, K.wrL, K.hdL), (K.elR, K.wrR, K.hdR)] {
            let f = simd_normalize(raw[wr] - raw[el])
            raw[hd] = raw[wr] + f * opts.handLength
        }
        raw[K.ftL] = raw[K.anL] + SIMD3<Float>(0, -0.06, -0.10)
        raw[K.ftR] = raw[K.anR] + SIMD3<Float>(0, -0.06, -0.10)

        // Wrists: blend in measured depth where it agrees with the pose.
        if opts.depthBlend > 0 {
            for (wr, hd) in [(K.wrL, K.hdL), (K.wrR, K.hdR)] {
                guard let p = pix[wr] else { continue }
                let b = depthBox(frames.depth, u0: p.x - 3, u1: p.x + 3, v0: p.y - 3, v1: p.y + 3)
                if b.count > 4 && abs(b.median - raw[wr].z) < 0.25 {
                    let dz = opts.depthBlend * ((b.median + 0.03) - raw[wr].z)
                    raw[wr].z += dz; raw[hd].z += dz
                }
            }
        }

        // 2D confidences give the joint states (Vision 3D has no per-joint confidence).
        var conf = [Int: Float]()
        if let o2 = request2D.results?.first {
            let map: [(Int, VNHumanBodyPoseObservation.JointName)] = [
                (K.shL, .leftShoulder), (K.elL, .leftElbow), (K.wrL, .leftWrist),
                (K.shR, .rightShoulder), (K.elR, .rightElbow), (K.wrR, .rightWrist),
                (K.hipL, .leftHip), (K.knL, .leftKnee), (K.anL, .leftAnkle),
                (K.hipR, .rightHip), (K.knR, .rightKnee), (K.anR, .rightAnkle),
                (K.hipC, .root), (K.shoulderC, .neck), (K.head, .nose)]
            for (k, n) in map { if let p = try? o2.recognizedPoint(n) { conf[k] = p.confidence } }
        }
        conf[K.hdL] = conf[K.wrL]; conf[K.hdR] = conf[K.wrR]
        conf[K.ftL] = conf[K.anL]; conf[K.ftR] = conf[K.anR]; conf[K.spine] = conf[K.hipC]
        for j in 0..<20 {
            let c = conf[j] ?? 0.5
            states[j] = c > 0.5 ? 2 : (c > 0.2 ? 1 : 0)
        }
        return (conf[K.shL] ?? 1) > 0.2 && (conf[K.shR] ?? 1) > 0.2
    }

    /// Learns bone lengths and keeps them constant (parent -> child), roots left as measured.
    func lockBones(_ j: [SIMD3<Float>], _ states: [UInt8]) -> [SIMD3<Float>] {
        var out = j
        for (a, b) in boneChains {
            let v = j[b] - j[a], n = simd_length(v)
            if n < 1e-6 { continue }
            if states[a] == 2 && states[b] == 2 {
                lengths[a * 32 + b] = lengths[a * 32 + b].map { 0.98 * $0 + 0.02 * n } ?? n
            }
            if boneCount >= 45, let len = lengths[a * 32 + b] { out[b] = out[a] + (v / n) * len }
        }
        boneCount += 1
        return out
    }
}

// MARK: - 3D point cloud view (SceneKit)

final class CloudView {
    let scn = SCNView(frame: .zero)
    let cloudNode = SCNNode()
    var boneNodes: [SCNNode] = [], jointNodes: [SCNNode] = []

    init() {
        let scene = SCNScene()
        scn.scene = scene
        scn.backgroundColor = NSColor(calibratedWhite: 0.05, alpha: 1)
        scn.allowsCameraControl = true
        scn.antialiasingMode = .multisampling4X
        let cam = SCNNode(); cam.camera = SCNCamera(); cam.camera!.zNear = 0.05; cam.camera!.zFar = 30; cam.camera!.fieldOfView = 55
        cam.position = SCNVector3(1.9, 0.9, -0.2)
        scene.rootNode.addChildNode(cam)
        cam.look(at: SCNVector3(0, 0, -2.2))
        scn.pointOfView = cam
        scn.defaultCameraController.target = SCNVector3(0, 0, -2.2)
        scene.rootNode.addChildNode(cloudNode)
        // the sensor itself: a small box at the origin
        let sensor = SCNNode(geometry: SCNBox(width: 0.28, height: 0.05, length: 0.06, chamferRadius: 0))
        sensor.geometry!.firstMaterial!.diffuse.contents = NSColor.systemGreen
        scene.rootNode.addChildNode(sensor)
        for _ in bones {
            let n = SCNNode(geometry: SCNCylinder(radius: 0.016, height: 1))
            n.geometry!.firstMaterial!.diffuse.contents = NSColor.cyan
            n.geometry!.firstMaterial!.lightingModel = .constant
            scene.rootNode.addChildNode(n); boneNodes.append(n)
        }
        for j in 0..<20 {
            let r: CGFloat = j == K.head ? 0.07 : 0.03
            let n = SCNNode(geometry: SCNSphere(radius: r))
            n.geometry!.firstMaterial!.diffuse.contents = [K.hdL, K.hdR].contains(j) ? NSColor.yellow : NSColor.white
            n.geometry!.firstMaterial!.lightingModel = .constant
            scene.rootNode.addChildNode(n); jointNodes.append(n)
        }
    }

    /// Kinect sensor space (x to the sensor's left) -> scene (x right, y up, camera looks down -z).
    static func scene(_ p: SIMD3<Float>) -> SCNVector3 { SCNVector3(-p.x, p.y, -p.z) }

    func update(frames: Frames, image: Data?, skeleton s: Snapshot) {
        // point cloud: every 2nd depth pixel, coloured from the registered colour pixel
        if let image {
            var pos = [Float](), col = [Float]()
            pos.reserveCapacity(80_000 * 3); col.reserveCapacity(80_000 * 4)
            let depth = frames.depth
            image.withUnsafeBytes { raw in
                let bgra = raw.bindMemory(to: UInt8.self)
                for v in stride(from: 0, to: H, by: 2) {
                    for u in stride(from: 0, to: W, by: 2) {
                        let m = rawToMetres[Int(min(depth[v * W + u], 2047))]
                        if m < 0.6 || m > 4.5 { continue }
                        pos.append(-(CX - Float(u)) * m / FX_D); pos.append((CY - Float(v)) * m / FX_D); pos.append(-m)
                        let cu = Int(CX + (Float(u) - CX) / D_SCALE), cv = Int(CY + (Float(v) - CY) / D_SCALE)
                        if cu >= 0 && cu < W && cv >= 0 && cv < H {
                            let o = (cv * W + cu) * 4
                            col.append(Float(bgra[o + 2]) / 255); col.append(Float(bgra[o + 1]) / 255); col.append(Float(bgra[o]) / 255)
                        } else { col.append(0.3); col.append(0.3); col.append(0.3) }
                        col.append(1)
                    }
                }
            }
            let n = pos.count / 3
            if n > 0 {
                let vsrc = SCNGeometrySource(data: pos.withUnsafeBytes { Data($0) }, semantic: .vertex, vectorCount: n,
                                             usesFloatComponents: true, componentsPerVector: 3, bytesPerComponent: 4,
                                             dataOffset: 0, dataStride: 12)
                let csrc = SCNGeometrySource(data: col.withUnsafeBytes { Data($0) }, semantic: .color, vectorCount: n,
                                             usesFloatComponents: true, componentsPerVector: 4, bytesPerComponent: 4,
                                             dataOffset: 0, dataStride: 16)
                let idx = (0..<UInt32(n)).map { $0 }
                let element = SCNGeometryElement(data: idx.withUnsafeBytes { Data($0) }, primitiveType: .point,
                                                 primitiveCount: n, bytesPerIndex: 4)
                element.pointSize = 3; element.minimumPointScreenSpaceRadius = 1; element.maximumPointScreenSpaceRadius = 3
                let g = SCNGeometry(sources: [vsrc, csrc], elements: [element])
                g.firstMaterial!.lightingModel = .constant
                cloudNode.geometry = g
            }
        }
        // skeleton
        for (n, _) in boneNodes.enumerated() { boneNodes[n].isHidden = !s.tracked }
        for n in jointNodes { n.isHidden = !s.tracked }
        guard s.tracked else { return }
        for (i, (a, b)) in bones.enumerated() {
            let pa = SIMD3<Float>(-s.filtered[a].x, s.filtered[a].y, -s.filtered[a].z)
            let pb = SIMD3<Float>(-s.filtered[b].x, s.filtered[b].y, -s.filtered[b].z)
            let d = pb - pa, len = simd_length(d)
            if len < 1e-4 { continue }
            boneNodes[i].simdPosition = (pa + pb) / 2
            boneNodes[i].simdScale = SIMD3<Float>(1, len, 1)
            boneNodes[i].simdOrientation = simd_quatf(from: SIMD3<Float>(0, 1, 0), to: d / len)
        }
        for (j, p) in s.filtered.enumerated() { jointNodes[j].simdPosition = SIMD3<Float>(-p.x, p.y, -p.z) }
    }
}

// MARK: - viewer

final class ViewerView: NSView {
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with event: NSEvent) {
        switch event.charactersIgnoringModifiers ?? "" {
        case "[": tuneCutoff = max(tuneCutoff * 0.8, 0.1)
        case "]": tuneCutoff = min(tuneCutoff * 1.25, 30)
        case ",": tuneBeta = max(tuneBeta * 0.8, 0.1)
        case ".": tuneBeta = min(tuneBeta * 1.25, 200)
        case ";": tuneZCutoff = max(tuneZCutoff * 0.8, 0.05)
        case "'": tuneZCutoff = min(tuneZCutoff * 1.25, 10)
        case "s": sendEnabled.toggle()
        case "r": tuneCutoff = 3.0; tuneBeta = 12.0; tuneZCutoff = 0.8
        default: break
        }
    }

    func projectColour(_ p: SIMD3<Float>) -> CGPoint { CGPoint(x: Double(CX - p.x * FX_C / p.z), y: Double(CY - p.y * FX_C / p.z)) }
    func projectDepth(_ p: SIMD3<Float>) -> CGPoint { CGPoint(x: Double(CX - p.x * FX_D / p.z), y: Double(CY - p.y * FX_D / p.z)) }
    /// Vision's colour-pixel coordinates -> depth-pixel coordinates.
    func colourToDepth(_ p: CGPoint) -> CGPoint {
        CGPoint(x: Double(CX) + (p.x - Double(CX)) * Double(D_SCALE), y: Double(CY) + (p.y - Double(CY)) * Double(D_SCALE))
    }

    func drawImage(_ ctx: CGContext, _ data: Data?, origin: CGPoint) {
        guard let data, let provider = CGDataProvider(data: data as CFData) else { return }
        let info = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue
        guard let img = CGImage(width: W, height: H, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: W * 4,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: info),
                                provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return }
        ctx.saveGState(); ctx.translateBy(x: origin.x, y: origin.y + CGFloat(H)); ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: W, height: H)); ctx.restoreGState()
    }

    func drawSkeleton(_ ctx: CGContext, _ s: Snapshot, origin: CGPoint, project: (SIMD3<Float>) -> CGPoint) {
        guard s.tracked else { return }
        ctx.setLineWidth(2.5); ctx.setStrokeColor(NSColor.white.cgColor)
        for (a, b) in bones {
            let pa = project(s.filtered[a]), pb = project(s.filtered[b])
            ctx.move(to: CGPoint(x: origin.x + pa.x, y: origin.y + pa.y))
            ctx.addLine(to: CGPoint(x: origin.x + pb.x, y: origin.y + pb.y)); ctx.strokePath()
        }
        for (j, p) in s.filtered.enumerated() {
            ctx.setFillColor((s.states[j] == 2 ? NSColor.white : NSColor.gray).cgColor)
            let q = project(p); ctx.fillEllipse(in: CGRect(x: origin.x + q.x - 4, y: origin.y + q.y - 4, width: 8, height: 8))
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor(calibratedWhite: 0.08, alpha: 1).cgColor); ctx.fill(bounds)
        snapshotLock.lock(); let s = snapshot; snapshotLock.unlock()
        let colourOrigin = CGPoint.zero, depthOrigin = CGPoint.zero
        let k: CGFloat = 0.75   // colour and depth panels are drawn at 75 % so three views fit

        // colour image: Vision's raw joints (green) + final skeleton
        ctx.saveGState(); ctx.translateBy(x: 0, y: 0); ctx.scaleBy(x: k, y: k)
        drawImage(ctx, s.image, origin: colourOrigin)
        ctx.setFillColor(NSColor.green.cgColor)
        for p in s.visionJoints2D { ctx.fillEllipse(in: CGRect(x: p.x - 3, y: p.y - 3, width: 6, height: 6)) }
        drawSkeleton(ctx, s, origin: colourOrigin, project: projectColour)
        ctx.restoreGState()

        // depth map: same skeleton re-projected through the depth camera, Vision joints mapped to depth pixels
        ctx.saveGState(); ctx.translateBy(x: 480, y: 0); ctx.scaleBy(x: k, y: k)
        drawImage(ctx, s.depthImage, origin: depthOrigin)
        ctx.setFillColor(NSColor.green.cgColor)
        for p in s.visionJoints2D {
            let q = colourToDepth(p); ctx.fillEllipse(in: CGRect(x: depthOrigin.x + q.x - 3, y: q.y - 3, width: 6, height: 6))
        }
        drawSkeleton(ctx, s, origin: depthOrigin, project: projectDepth)
        if let box = s.torsoBox {
            let o = colourToDepth(box.origin)
            ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.8).cgColor); ctx.setLineWidth(1)
            ctx.stroke(CGRect(x: depthOrigin.x + o.x, y: o.y, width: box.width * Double(D_SCALE), height: box.height * Double(D_SCALE)))
        }
        ctx.restoreGState()
        for (label, x) in [("colour: green = Vision joints, white = final skeleton", CGFloat(6)),
                           ("depth (red near, blue far): white = skeleton, box = torso sample", CGFloat(486)),
                           ("3D point cloud + skeleton (drag to rotate, scroll to zoom)", CGFloat(966))] {
            NSAttributedString(string: label, attributes: [.foregroundColor: NSColor.white, .backgroundColor: NSColor(calibratedWhite: 0, alpha: 0.6), .font: NSFont.systemFont(ofSize: 11)])
                .draw(at: CGPoint(x: x, y: 6))
        }

        // front + side views (raw thin red, filtered cyan) and head depth graph
        func panel(_ rect: CGRect, title: String, point: (SIMD3<Float>) -> CGPoint) {
            ctx.setFillColor(NSColor(calibratedWhite: 0.14, alpha: 1).cgColor); ctx.fill(rect)
            NSAttributedString(string: title, attributes: [.foregroundColor: NSColor.lightGray, .font: NSFont.systemFont(ofSize: 11)])
                .draw(at: CGPoint(x: rect.minX + 6, y: rect.minY + 4))
            guard s.tracked else { return }
            for (joints, color, width) in [(s.raw, NSColor.red.withAlphaComponent(0.5), CGFloat(1)),
                                           (s.filtered, NSColor.cyan, CGFloat(2.5))] {
                ctx.setStrokeColor(color.cgColor); ctx.setLineWidth(width)
                for (a, b) in bones {
                    let pa = point(joints[a]), pb = point(joints[b])
                    ctx.move(to: CGPoint(x: rect.minX + pa.x, y: rect.minY + pa.y))
                    ctx.addLine(to: CGPoint(x: rect.minX + pb.x, y: rect.minY + pb.y)); ctx.strokePath()
                }
            }
        }
        let hip = s.filtered[0], scale: Float = 120
        panel(CGRect(x: 0, y: 370, width: 260, height: 250), title: "front view") { p in
            CGPoint(x: Double(130 - (p.x - hip.x) * scale), y: Double(190 - (p.y - hip.y) * scale))
        }
        panel(CGRect(x: 270, y: 370, width: 260, height: 250), title: "side view (z away)") { p in
            CGPoint(x: Double(130 + (p.z - hip.z) * scale), y: Double(190 - (p.y - hip.y) * scale))
        }
        let g = CGRect(x: 540, y: 370, width: 900, height: 250)
        ctx.setFillColor(NSColor(calibratedWhite: 0.14, alpha: 1).cgColor); ctx.fill(g)
        NSAttributedString(string: "head depth: raw (red) vs filtered (cyan), 20 cm span", attributes: [.foregroundColor: NSColor.lightGray, .font: NSFont.systemFont(ofSize: 11)])
            .draw(at: CGPoint(x: g.minX + 6, y: g.minY + 4))
        if s.headZFiltered.count > 2, let mid = s.headZFiltered.last {
            for (series, color) in [(s.headZRaw, NSColor.red.withAlphaComponent(0.6)), (s.headZFiltered, NSColor.cyan)] {
                ctx.setStrokeColor(color.cgColor); ctx.setLineWidth(1.5)
                for (i, v) in series.enumerated() {
                    let pt = CGPoint(x: g.minX + CGFloat(i) / 180 * g.width, y: g.midY - CGFloat((v - mid) / 0.2) * g.height)
                    i == 0 ? ctx.move(to: pt) : ctx.addLine(to: pt)
                }
                ctx.strokePath()
            }
        }

        let text = String(format: "%.0f fps   pipeline %.1f ms (vision %.1f ms)   tracked %.0f%%   %@   %@",
                          s.fps, s.pipelineMs, s.visionMs, s.trackedPercent,
                          s.tracked ? String(format: "hip z %.2f m", s.filtered[0].z) : "NO PLAYER",
                          sendEnabled ? "SENDING to Xenia (\(s.sent))  [s to stop]" : "viewer only  [s to send]")
        NSAttributedString(string: text, attributes: [.foregroundColor: NSColor.white, .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)])
            .draw(at: CGPoint(x: 10, y: 625))
        NSAttributedString(string: String(format: "smoothing at rest %.2f Hz [ ]    speed response %.1f , .    depth %.2f Hz ; '    r reset",
                                          tuneCutoff, tuneBeta, tuneZCutoff),
                           attributes: [.foregroundColor: NSColor.systemYellow, .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)])
            .draw(at: CGPoint(x: 10, y: 643))
    }
}

// MARK: - main

guard let frames = Frames() else {
    print("Cannot open /tmp/xenia_kinect_frames. Start Xenia with --kinect_share_frames or harness/kinect_publish.")
    exit(1)
}
let pipeline = Pipeline(frames: frames)
Thread { pipeline.run() }.start()

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let window = NSWindow(contentRect: NSRect(x: 40, y: 60, width: 1450, height: 668),
                      styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
window.title = "Kinect native tracker"
let view = ViewerView(frame: window.contentView!.bounds)
view.autoresizingMask = [.width, .height]
window.contentView = view
let cloud = CloudView()
cloud.scn.frame = NSRect(x: 960, y: 0, width: 480, height: 360)
view.addSubview(cloud.scn)
window.makeKeyAndOrderFront(nil)
window.makeFirstResponder(view)
app.activate(ignoringOtherApps: true)
Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { _ in view.needsDisplay = true }
Timer.scheduledTimer(withTimeInterval: 1.0 / 12, repeats: true) { _ in
    snapshotLock.lock(); let s = snapshot; snapshotLock.unlock()
    cloud.update(frames: frames, image: s.image, skeleton: s)
}
app.run()
