import os
import shutil
import dataclasses
from typing import Dict, Any, Optional

@dataclasses.dataclass
class Manifest:
    """Represents the bunker.toml configuration"""
    name: str
    version: str
    authors: list[str]
    edition: str = "2026"

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> 'Manifest':
        package = data.get('package', {})
        return cls(
            name=package.get('name', 'unknown'),
            version=package.get('version', '0.1.0'),
            authors=package.get('authors', []),
            edition=package.get('edition', '2026')
        )

    def to_toml(self) -> str:
        authors_str = str(self.authors).replace("'", '"')
        return f"""[package]
name = "{self.name}"
version = "{self.version}"
authors = {authors_str}
edition = "{self.edition}"

[dependencies]
"""

class PackageManager:
    """Handles project creation and management"""
    
    @staticmethod
    def create_project(name: str, path: str = ".") -> bool:
        """Scaffolds a new Bunker project"""
        project_dir = os.path.join(path, name)
        
        if os.path.exists(project_dir):
            print(f"Error: Directory '{project_dir}' already exists.")
            return False
        
        try:
            # Create directories
            os.makedirs(os.path.join(project_dir, "src"), exist_ok=True)
            os.makedirs(os.path.join(project_dir, "tests"), exist_ok=True)
            
            # Create bunker.toml
            manifest = Manifest(name=name, version="0.1.0", authors=["User"])
            with open(os.path.join(project_dir, "bunker.toml"), "w") as f:
                f.write(manifest.to_toml())
            
            # Create src/main.bnk
            with open(os.path.join(project_dir, "src", "main.bnk"), "w") as f:
                f.write(f"""module {name}

Entity {name.capitalize()}App:
    has a public method main:
        print: "Hello, Bunker v2.6.0!".
    .
.
""")
            
            # Create .gitignore
            with open(os.path.join(project_dir, ".gitignore"), "w") as f:
                f.write("/target\n/build\n**/*.o\n")
                
            print(f"Created binary (application) `{name}` package")
            return True
            
        except Exception as e:
            print(f"Error creating project: {e}")
            # Cleanup
            if os.path.exists(project_dir):
                shutil.rmtree(project_dir)
            return False

    @staticmethod
    def read_manifest(path: str) -> Optional[Manifest]:
        """Reads and parses bunker.toml"""
        manifest_path = os.path.join(path, "bunker.toml")
        if not os.path.exists(manifest_path):
            return None
            
        try:
            # Simple TOML parser for now (avoiding external deps for prototype)
            data = {"package": {}}
            current_section = None
            
            with open(manifest_path, "r") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                        
                    if line.startswith("[") and line.endswith("]"):
                        current_section = line[1:-1]
                        if current_section not in data:
                            data[current_section] = {}
                        continue
                        
                    if "=" in line:
                        key, value = [p.strip() for p in line.split("=", 1)]
                        # Remove quotes
                        if value.startswith('"') and value.endswith('"'):
                            value = value[1:-1]
                        elif value.startswith('[') and value.endswith(']'):
                            # Simple list parse
                            value = [v.strip().strip('"') for v in value[1:-1].split(',') if v.strip()]
                            
                        if current_section:
                            data[current_section][key] = value
                            
            return Manifest.from_dict(data)
            
        except Exception as e:
            print(f"Error reading manifest: {e}")
            return None
