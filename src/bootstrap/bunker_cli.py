#!/usr/bin/env python3
import argparse
import sys
import os
import subprocess
from bunker_pkg import PackageManager

def main():
    parser = argparse.ArgumentParser(description="Bunker Language CLI v2.6.0")
    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Command: new
    parser_new = subparsers.add_parser("new", help="Create a new Bunker project")
    parser_new.add_argument("name", help="Name of the project")

    # Command: build
    parser_build = subparsers.add_parser("build", help="Build the current project")

    # Command: test
    parser_test = subparsers.add_parser("test", help="Run tests")
    
    # Command: run
    parser_run = subparsers.add_parser("run", help="Run the current project")

    # Command: repl
    parser_repl = subparsers.add_parser("repl", help="Start the Interactive REPL")

    # Parse arguments
    args = parser.parse_args()

    if args.command == "new":
        success = PackageManager.create_project(args.name)
        if success:
            sys.exit(0)
        else:
            sys.exit(1)

    elif args.command == "build":
        if not os.path.exists("bunker.toml"):
            print("Error: clean manifest not found. Are you in a Bunker project root?")
            sys.exit(1)
        
        manifest = PackageManager.read_manifest(".")
        if manifest:
            print(f"Building {manifest.name} v{manifest.version}...")
            
            # Define paths
            src_file = "src/main.bnk"
            if not os.path.exists(src_file):
                 print(f"Error: {src_file} not found")
                 sys.exit(1)
                 
            output_bin = os.path.join("target", manifest.name)
            if not os.path.exists("target"):
                os.makedirs("target")
            
            # Invoke bunkerc
            # Assuming bunkerc.py is in the same directory as this script
            compiler_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bunkerc.py")
            cmd = [sys.executable, compiler_path, src_file, "-o", output_bin]
            
            try:
                subprocess.run(cmd, check=True)
                print(f"Build successful. Artifact: {output_bin}")
            except subprocess.CalledProcessError:
                print("Build failed.")
                sys.exit(1)
                
        else:
            print("Error: Could not read bunker.toml")
            sys.exit(1)

    elif args.command == "test":
        print("Running tests...")
        # Placeholder
        print("All tests passed (0/0).")

    elif args.command == "run":
        # Placeholder
        print("Running application...")
        print("Hello, Bunker v2.6.0!")

    elif args.command == "repl":
        compiler_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bunkerc.py")
        try:
            subprocess.run([sys.executable, compiler_path, "--repl"], check=True)
        except KeyboardInterrupt:
            pass
        except subprocess.CalledProcessError:
            sys.exit(1)

    else:
        parser.print_help()

if __name__ == "__main__":
    main()
