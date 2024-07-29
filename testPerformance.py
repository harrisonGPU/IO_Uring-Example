import os
import random
import string
import subprocess
import re

def generate_random_text(size):
    """Generate a random string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=size))

def create_random_file(directory):
    """Create a text file."""
    file_size = random.randint(1024, 4096)  # File size between 1KB and 4KB
    content = generate_random_text(file_size)
    if not os.path.exists(directory):
        os.makedirs(directory)
    filename = os.path.join(directory, f"random_file_{random.randint(1000,9999)}.txt")
    with open(filename, 'w') as f:
        f.write(content)
    return filename, content

def run_my_cat(file_path, original_content):
    """Execute the 'my_cat' program."""
    try:
        result = subprocess.run(['./build/my_cat', file_path], capture_output=True, text=True)
        output = result.stdout
        
        # Extract the completion message and then remove it from the output
        completion_match = re.search(r'\nCompleted reading.*?bytes\.\n', output)
        completion_message = completion_match.group(0) if completion_match else ""
        cleaned_output = re.sub(r'\nCompleted reading.*?bytes\.\n', '', output)
        
        # Remove non-letter characters from the end of the cleaned output
        cleaned_output = re.sub(r'\W+$', '', cleaned_output)
        
        if cleaned_output == original_content:
            print(completion_message.strip())
            print("Successful: Content matches.")
            os.remove(file_path)
            return True
        else:
            print("Fail: Content does not match.")
            print("Detail comparison:")
            print("Expected:", original_content)
            print("Received:", cleaned_output)
            return False
    except Exception as e:
        print("An error occurred:", str(e))
        return False

def main():
    directory = os.getenv('PWD')
    num_files = int(input("Enter the number of files to generate and test: "))
    for _ in range(num_files):
        file_path, content = create_random_file(directory)
        print("File created at:", file_path)
        if not run_my_cat(file_path, content):
            break 

if __name__ == "__main__":
    main()
