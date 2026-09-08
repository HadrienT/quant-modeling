import { useEffect, useMemo, useRef, useState } from "react";
import * as THREE from "three";
import { Canvas, useThree } from "@react-three/fiber";
import { Html, OrbitControls } from "@react-three/drei";
import { useChartTheme } from "../theme";
import { type SurfaceGrid, zExtent } from "./SurfaceGrid";
import { buildSurfaceGeometry, nearestNode } from "./geometry";
import {
	buildLut,
	surfaceFragmentShader,
	surfaceVertexShader,
} from "./surfaceMaterial";

export type PresetView = "three-quarter" | "front" | "profile" | "top";

const CAMERA_POS: Record<PresetView, [number, number, number]> = {
	"three-quarter": [2.2, 1.9, 2.6],
	front: [0, 0.7, 3.4],
	profile: [3.4, 0.7, 0],
	top: [0, 3.6, 0.001],
};

function SurfaceMesh({
	grid,
	mode,
	showDataGrid,
	onHover,
}: {
	grid: SurfaceGrid;
	mode: "sequential" | "divergent";
	showDataGrid: boolean;
	onHover: (node: ReturnType<typeof nearestNode> | null) => void;
}) {
	const tokens = useChartTheme();
	const { invalidate } = useThree();

	const geom = useMemo(() => buildSurfaceGeometry(grid), [grid]);

	const geometry = useMemo(() => {
		const g = new THREE.BufferGeometry();
		g.setAttribute("position", new THREE.BufferAttribute(geom.positions, 3));
		g.setAttribute("normal", new THREE.BufferAttribute(geom.normals, 3));
		g.setAttribute("aValue", new THREE.BufferAttribute(geom.uv.filter((_, i) => i % 2 === 1), 1));
		g.setIndex(new THREE.BufferAttribute(geom.indices, 1));
		return g;
	}, [geom]);

	const material = useMemo(() => {
		const lut = buildLut(tokens, mode);
		const [zMin, zMax] = zExtent(grid);
		const isoStep = zMax > zMin ? 0.1 : 0.1; // 10 levels across the range
		return new THREE.ShaderMaterial({
			vertexShader: surfaceVertexShader,
			fragmentShader: surfaceFragmentShader,
			side: THREE.DoubleSide,
			uniforms: {
				uLut: { value: lut },
				uLightDir: { value: new THREE.Vector3(2, 3, 1.5).normalize() },
				uAmbient: { value: new THREE.Color(tokens.canvas).multiplyScalar(1.2) },
				uIsoStep: { value: isoStep },
				uIsoColor: { value: new THREE.Color(tokens.ink) },
				uShowGridLines: { value: showDataGrid ? 1 : 0 },
			},
		});
	}, [tokens, mode, grid, showDataGrid]);

	useEffect(() => {
		invalidate();
		return () => {
			geometry.dispose();
			material.dispose();
			(material.uniforms.uLut!.value as THREE.Texture).dispose();
		};
	}, [geometry, material, invalidate]);

	return (
		<group>
			<mesh
				geometry={geometry}
				material={material}
				onPointerMove={(e) => {
					e.stopPropagation();
					if (!e.point) return;
					onHover(nearestNode(grid, e.point.x, e.point.z));
				}}
				onPointerOut={() => onHover(null)}
			/>
			{/* base plane + soft shadow catcher */}
			<mesh rotation-x={-Math.PI / 2} position-y={-0.02} receiveShadow>
				<planeGeometry args={[3, 3]} />
				<meshStandardMaterial
					color={tokens.surface}
					roughness={1}
					metalness={0}
				/>
			</mesh>
		</group>
	);
}

function Rig({ view }: { view: PresetView }) {
	const { camera } = useThree();
	useEffect(() => {
		const [x, y, z] = CAMERA_POS[view];
		camera.position.set(x, y, z);
		camera.lookAt(0, 0.25, 0);
	}, [view, camera]);
	return null;
}

export function SurfaceScene({
	grid,
	mode = "sequential",
	view = "three-quarter",
	showDataGrid = false,
	reducedMotion = false,
}: {
	grid: SurfaceGrid;
	mode?: "sequential" | "divergent";
	view?: PresetView;
	showDataGrid?: boolean;
	reducedMotion?: boolean;
}) {
	const tokens = useChartTheme();
	const [hover, setHover] = useState<ReturnType<typeof nearestNode> | null>(
		null,
	);
	const glRef = useRef<THREE.WebGLRenderer>(null);

	return (
		<Canvas
			frameloop="demand"
			dpr={[1, 2]}
			camera={{ fov: 40, position: CAMERA_POS[view], near: 0.1, far: 100 }}
			gl={{ antialias: true, preserveDrawingBuffer: true }}
			onCreated={({ gl }) => {
				gl.toneMapping = THREE.ACESFilmicToneMapping;
				glRef.current = gl;
			}}
			style={{ background: tokens.canvas }}
		>
			<color attach="background" args={[tokens.canvas]} />
			<ambientLight intensity={0.5} />
			<hemisphereLight
				intensity={0.6}
				color={tokens.ink}
				groundColor={tokens.canvas}
			/>
			<directionalLight position={[3, 5, 2]} intensity={1.1} castShadow />
			<directionalLight position={[-4, 2, -3]} intensity={0.35} />

			<Rig view={view} />
			<SurfaceMesh
				grid={grid}
				mode={mode}
				showDataGrid={showDataGrid}
				onHover={setHover}
			/>

			{hover && !Number.isNaN(hover.z) && (
				<Html
					position={[
						(hover.xi / Math.max(1, grid.x.length - 1)) * 2 - 1,
						0.6,
						(hover.yi / Math.max(1, grid.y.length - 1)) * 2 - 1,
					]}
					className="pointer-events-none rounded-sm border border-hairline bg-surface px-1.5 py-1 font-mono text-2xs text-ink shadow"
				>
					{grid.axes.x.label} {grid.axes.x.format?.(hover.x) ?? hover.x}
					<br />
					{grid.axes.y.label} {grid.axes.y.format?.(hover.y) ?? hover.y}
					<br />
					<span className="text-accent">
						{grid.axes.z.format?.(hover.z) ?? hover.z.toFixed(4)}
					</span>
				</Html>
			)}

			<OrbitControls
				enableDamping={!reducedMotion}
				dampingFactor={0.08}
				minPolarAngle={0.15}
				maxPolarAngle={Math.PI / 2 - 0.05}
				minDistance={1.6}
				maxDistance={7}
				makeDefault
			/>
		</Canvas>
	);
}

export function exportSceneToPng(): string | null {
	const canvas = document.querySelector("canvas");
	return canvas ? canvas.toDataURL("image/png") : null;
}
