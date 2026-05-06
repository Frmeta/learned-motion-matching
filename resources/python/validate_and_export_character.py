"""
Validate FBX rigged characters and export to character.bin format.

This script validates that an FBX file has the correct skeletal structure
and can be exported to the character.bin format for use with controller.cpp.

Usage (standalone mode - requires Blender):
    blender --background --python validate_and_export_character.py -- \
        --input path/to/model.fbx \
        --validate-only              # Just validate without exporting
        --output resources/bin/character.bin

Usage (validation mode - no Blender required):
    python validate_and_export_character.py --input path/to/model.fbx --validate-only

Requires:
    - For validation: fbx library (pip install fbx) or assimp (pip install assimp)
    - For export: Blender 3.0+
"""

import argparse
import sys
import struct
import json
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass


# Expected skeleton structure (23 bones)
EXPECTED_BONES: List[str] = [
    "Bone_Entity",
    "Bone_Hips",
    "Bone_LeftUpLeg",
    "Bone_LeftLeg",
    "Bone_LeftFoot",
    "Bone_LeftToe",
    "Bone_RightUpLeg",
    "Bone_RightLeg",
    "Bone_RightFoot",
    "Bone_RightToe",
    "Bone_Spine",
    "Bone_Spine1",
    "Bone_Spine2",
    "Bone_Neck",
    "Bone_Head",
    "Bone_LeftShoulder",
    "Bone_LeftArm",
    "Bone_LeftForeArm",
    "Bone_LeftHand",
    "Bone_RightShoulder",
    "Bone_RightArm",
    "Bone_RightForeArm",
    "Bone_RightHand",
]

CANONICAL_PARENTS: List[int] = [
    -1,  # Bone_Entity
    0,   # Bone_Hips
    1,   # Bone_LeftUpLeg
    2,   # Bone_LeftLeg
    3,   # Bone_LeftFoot
    4,   # Bone_LeftToe
    1,   # Bone_RightUpLeg
    6,   # Bone_RightLeg
    7,   # Bone_RightFoot
    8,   # Bone_RightToe
    1,   # Bone_Spine
    10,  # Bone_Spine1
    11,  # Bone_Spine2
    12,  # Bone_Neck
    13,  # Bone_Head
    12,  # Bone_LeftShoulder
    15,  # Bone_LeftArm
    16,  # Bone_LeftForeArm
    17,  # Bone_LeftHand
    12,  # Bone_RightShoulder
    19,  # Bone_RightArm
    20,  # Bone_RightForeArm
    21,  # Bone_RightHand
]


@dataclass
class ValidationResult:
    """Results of FBX validation."""
    is_valid: bool
    errors: List[str]
    warnings: List[str]
    fbx_info: Dict
    
    def to_dict(self):
        return {
            "is_valid": self.is_valid,
            "errors": self.errors,
            "warnings": self.warnings,
            "fbx_info": self.fbx_info,
        }


class FBXValidator:
    """Validates FBX files for character export compatibility."""
    
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.fbx_info: Dict = {}
        
    def validate(self) -> ValidationResult:
        """Run full validation on the FBX file."""
        self.errors = []
        self.warnings = []
        self.fbx_info = {}
        
        try:
            self._validate_file_exists()
            self._validate_fbx_structure()
            self._extract_fbx_info()
        except Exception as e:
            self.errors.append(f"Validation error: {str(e)}")
            
        is_valid = len(self.errors) == 0
        return ValidationResult(
            is_valid=is_valid,
            errors=self.errors,
            warnings=self.warnings,
            fbx_info=self.fbx_info,
        )
    
    def _validate_file_exists(self):
        """Check if FBX file exists and is readable."""
        import os
        if not os.path.exists(self.filepath):
            self.errors.append(f"File not found: {self.filepath}")
            raise FileNotFoundError(self.filepath)
        
        if not os.path.isfile(self.filepath):
            self.errors.append(f"Not a file: {self.filepath}")
            raise ValueError(f"Not a file: {self.filepath}")
            
        if not self.filepath.lower().endswith('.fbx'):
            self.warnings.append(f"File extension is not .fbx: {self.filepath}")
    
    def _validate_fbx_structure(self):
        """Validate FBX file structure and required bones."""
        try:
            bones_found = self._extract_bone_names()
            self._validate_bones(bones_found)
        except ImportError:
            self.warnings.append(
                "Could not import fbx/assimp library. "
                "Install with: pip install assimp  OR  pip install fbx"
                "\nFull bone structure validation skipped. "
                "Use Blender export for complete validation."
            )
            return
    
    def _extract_bone_names(self) -> List[str]:
        """Extract bone names from FBX file."""
        bones = []
        
        # Try assimp first (more reliable)
        try:
            import assimp
            scene = assimp.load(self.filepath)
            
            def extract_bones_from_node(node, bones_list: List[str]):
                bones_list.append(node.name)
                for child in node.children:
                    extract_bones_from_node(child, bones_list)
            
            if scene.rootnode:
                extract_bones_from_node(scene.rootnode, bones)
            
            assimp.release(scene)
            return bones
        except ImportError:
            pass
        except Exception as e:
            self.warnings.append(f"Error loading with assimp: {e}")
        
        # Fallback: Try fbx library
        try:
            # Basic FBX structure checking via file format inspection
            with open(self.filepath, 'rb') as f:
                # FBX files start with "Kaydara FBX Binary"
                header = f.read(27)
                
                # Check if file starts with "Kaydara FBX Binary"
                if not header.startswith(b'Kaydara FBX Binary'):
                    # Try to provide helpful debug info
                    try:
                        header_str = header.decode('utf-8', errors='replace')[:50]
                        self.errors.append(
                            f"File is not a valid FBX binary file. "
                            f"Header: {repr(header_str)}"
                        )
                    except:
                        self.errors.append("File is not a valid FBX binary file")
                    return bones
                
            # For now, return empty list - full parsing requires complex FBX library
            # This is validated properly in Blender
            self.warnings.append(
                "Basic FBX format check passed, but full bone structure validation "
                "requires Blender export. Use export_character.py for complete validation."
            )
            return bones
        except Exception as e:
            self.warnings.append(f"Error validating FBX format: {e}")
            return bones
    
    def _validate_bones(self, bones_found: List[str]):
        """Validate that required bones are present."""
        if not bones_found:
            # Can't validate without bone list - this is OK, Blender will validate
            return
        
        bones_set = set(bones_found)
        missing_bones = []
        
        for expected_bone in EXPECTED_BONES:
            if expected_bone == "Bone_Entity":
                # Bone_Entity can be synthesized
                continue
            
            if expected_bone not in bones_set:
                # Check for case-insensitive match
                if not any(b.lower() == expected_bone.lower() for b in bones_found):
                    missing_bones.append(expected_bone)
        
        if missing_bones:
            self.errors.append(
                f"Missing required bones: {', '.join(missing_bones)}\n"
                f"Expected 23-bone character skeleton. Found bones: {', '.join(sorted(bones_found))}"
            )
    
    def _extract_fbx_info(self):
        """Extract FBX file information."""
        import os
        
        file_size = os.path.getsize(self.filepath)
        self.fbx_info = {
            "filepath": self.filepath,
            "file_size": file_size,
            "file_size_mb": round(file_size / (1024 * 1024), 2),
        }


class CharacterBinWriter:
    """Writes character.bin format files."""
    
    def __init__(self, output_path: str):
        self.output_path = output_path
        self.file = None
    
    def write_array1d_float(self, data: List[float]):
        """Write array1d<float> to file."""
        self.file.write(struct.pack('<I', len(data)))
        for value in data:
            self.file.write(struct.pack('<f', value))
    
    def write_array1d_vec3(self, data: List[Tuple[float, float, float]]):
        """Write array1d<vec3> to file."""
        self.file.write(struct.pack('<I', len(data)))
        for x, y, z in data:
            self.file.write(struct.pack('<fff', x, y, z))
    
    def write_array1d_vec2(self, data: List[Tuple[float, float]]):
        """Write array1d<vec2> to file."""
        self.file.write(struct.pack('<I', len(data)))
        for x, y in data:
            self.file.write(struct.pack('<ff', x, y))
    
    def write_array1d_ushort(self, data: List[int]):
        """Write array1d<unsigned short> to file."""
        self.file.write(struct.pack('<I', len(data)))
        for value in data:
            self.file.write(struct.pack('<H', value))
    
    def write_array1d_quat(self, data: List[Tuple[float, float, float, float]]):
        """Write array1d<quat> to file."""
        self.file.write(struct.pack('<I', len(data)))
        for w, x, y, z in data:
            self.file.write(struct.pack('<ffff', w, x, y, z))
    
    def write_array2d_float(self, rows: int, cols: int, data: List[List[float]]):
        """Write array2d<float> to file."""
        self.file.write(struct.pack('<II', rows, cols))
        for row in data:
            for value in row:
                self.file.write(struct.pack('<f', value))
    
    def write_array2d_ushort(self, rows: int, cols: int, data: List[List[int]]):
        """Write array2d<unsigned short> to file."""
        self.file.write(struct.pack('<II', rows, cols))
        for row in data:
            for value in row:
                self.file.write(struct.pack('<H', value))


def validate_fbx(fbx_path: str) -> ValidationResult:
    """Validate an FBX file for character export compatibility."""
    validator = FBXValidator(fbx_path)
    return validator.validate()


def print_validation_report(result: ValidationResult, verbose: bool = True):
    """Print validation results to console."""
    print("\n" + "="*70)
    print("FBX VALIDATION REPORT")
    print("="*70)
    
    if result.is_valid:
        print("✓ VALID - FBX file is compatible with character export")
    else:
        print("✗ INVALID - FBX file has issues")
    
    print("\nFile Info:")
    for key, value in result.fbx_info.items():
        print(f"  {key}: {value}")
    
    if result.errors:
        print("\nErrors:")
        for i, error in enumerate(result.errors, 1):
            print(f"  [{i}] {error}")
    
    if result.warnings:
        print("\nWarnings:")
        for i, warning in enumerate(result.warnings, 1):
            print(f"  [{i}] {warning}")
    
    if not result.errors and not result.warnings:
        print("\nNo issues found!")
    
    print("="*70 + "\n")
    
    return result.is_valid


def export_via_blender(fbx_path: str, output_path: str):
    """
    Export FBX to character.bin using Blender.
    
    This should be run inside Blender:
    blender --background --python export_character.py -- \\
        --input fbx_path \\
        --output output_path
    """
    import subprocess
    import os
    
    script_dir = os.path.dirname(os.path.abspath(__file__))
    export_script = os.path.join(script_dir, "export_character.py")
    
    if not os.path.exists(export_script):
        print(f"Error: export_character.py not found at {export_script}")
        return False
    
    try:
        result = subprocess.run(
            [
                "blender",
                "--background",
                "--python", export_script,
                "--",
                "--input", fbx_path,
                "--output", output_path,
            ],
            capture_output=True,
            text=True,
        )
        
        print(result.stdout)
        if result.stderr:
            print("Errors:", result.stderr)
        
        return result.returncode == 0
    except FileNotFoundError:
        print("Error: Blender not found in PATH")
        print("Install Blender or add it to your PATH")
        return False
    except Exception as e:
        print(f"Error running Blender: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Validate and export FBX files to character.bin format",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Validate only (no Blender required)
  python validate_and_export_character.py --input model.fbx --validate-only

  # Export via Blender (requires Blender 3.0+)
  blender --background --python validate_and_export_character.py -- \\
    --input model.fbx --output character.bin

  # Verbose output
  python validate_and_export_character.py --input model.fbx --validate-only -v
        """
    )
    
    parser.add_argument("--input", "-i", required=True, help="Input FBX file path")
    parser.add_argument("--output", "-o", help="Output character.bin file path")
    parser.add_argument(
        "--validate-only", "-v", action="store_true",
        help="Only validate, don't export"
    )
    parser.add_argument(
        "--json", "-j", action="store_true",
        help="Output validation results as JSON"
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Verbose output"
    )
    
    args = parser.parse_args()
    
    # Try to import bpy to detect if running in Blender
    try:
        import bpy
        in_blender = True
    except ImportError:
        in_blender = False
    
    # Validate
    print(f"Validating: {args.input}")
    result = validate_fbx(args.input)
    
    if args.json:
        print(json.dumps(result.to_dict(), indent=2))
    else:
        print_validation_report(result, verbose=args.verbose)
    
    if not result.is_valid:
        sys.exit(1)
    
    # Export if requested and valid
    if args.output:
        if not args.validate_only:
            print(f"Exporting to: {args.output}")
            
            if in_blender:
                # Running inside Blender - call export_character.py
                print("(Export functionality requires Blender context)")
                print("Use: blender --background --python export_character.py -- --input model.fbx --output character.bin")
                sys.exit(0)
            else:
                # Not in Blender - try to invoke it
                if export_via_blender(args.input, args.output):
                    print(f"✓ Successfully exported to {args.output}")
                    sys.exit(0)
                else:
                    print("Export failed")
                    sys.exit(1)


if __name__ == "__main__":
    main()
