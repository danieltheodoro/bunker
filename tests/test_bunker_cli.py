import unittest
import os
import shutil
import sys
import subprocess

class TestBunkerCLI(unittest.TestCase):
    def setUp(self):
        # Clean up any previous test artifacts
        if os.path.exists("test_project_1"):
            shutil.rmtree("test_project_1")
        
    def tearDown(self):
        # Clean up after test
        if os.path.exists("test_project_1"):
            shutil.rmtree("test_project_1")

    def test_new_project_scaffold(self):
        """Test 'bunker new' command"""
        cmd = [sys.executable, "bunker_cli.py", "new", "test_project_1"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        # Check exit code
        self.assertEqual(result.returncode, 0, f"Command failed: {result.stderr}")
        
        # Check directory structure
        self.assertTrue(os.path.exists("test_project_1"), "Project dir not created")
        self.assertTrue(os.path.exists("test_project_1/bunker.toml"), "Manifest not created")
        self.assertTrue(os.path.exists("test_project_1/src/main.bnk"), "Source file not created")
        self.assertTrue(os.path.exists("test_project_1/.gitignore"), "Gitignore not created")
        
        # Check manifest content
        with open("test_project_1/bunker.toml", "r") as f:
            content = f.read()
            self.assertIn('name = "test_project_1"', content)
            self.assertIn('edition = "2026"', content)

    def test_build_project(self):
        """Test 'bunker build' command (end-to-end)"""
        # 1. Create project
        subprocess.run([sys.executable, "bunker_cli.py", "new", "test_app_build"], check=True)
        
        # 2. Build project
        cwd = os.getcwd()
        os.chdir("test_app_build")
        try:
            # Run build
            cmd = [sys.executable, "../bunker_cli.py", "build"]
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, f"Build failed: {result.stderr}")
            self.assertIn("Build successful", result.stdout)
            
            # 3. Verify artifact
            target_bin = os.path.join("target", "test_app_build")
            self.assertTrue(os.path.exists(target_bin), "Binary not created")
            
            # 4. Run artifact
            run_result = subprocess.run([target_bin], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0)
            self.assertIn("Hello, Bunker v2.6.0!", run_result.stdout)
            
        finally:
            os.chdir(cwd)
            # Cleanup
            if os.path.exists("test_app_build"):
                shutil.rmtree("test_app_build")

if __name__ == '__main__':
    unittest.main()
