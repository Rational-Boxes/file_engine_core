#include <iostream>
#include <fstream>
#include <string>
#include <grpcpp/grpcpp.h>

// Include the generated gRPC files
#include "fileengine/fileservice.grpc.pb.h"

int main() {
    std::string server_address("localhost:50051");
    
    // Create a gRPC channel to connect to the server
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    auto stub = fileengine::FileService::NewStub(channel);

    std::cout << "Connected to FileEngine gRPC server" << std::endl;

    // Create a simple test file
    std::string test_content = "This is a test file for S3 synchronization.";
    std::string file_path = "/test_file.txt";
    
    // Create a Put request
    fileengine::PutRequest put_request;
    put_request.set_path(file_path);
    put_request.set_content(test_content);
    put_request.set_username("test_user");
    
    // Add a role for permissions
    put_request.add_roles("admin");

    fileengine::PutResponse put_response;
    grpc::ClientContext context;

    std::cout << "Attempting to upload file to server..." << std::endl;
    
    // Make the gRPC call
    grpc::Status status = stub->Put(&context, put_request, &put_response);

    if (status.ok()) {
        std::cout << "File uploaded successfully!" << std::endl;
        std::cout << "Response: " << put_response.message() << std::endl;
    } else {
        std::cout << "File upload failed!" << std::endl;
        std::cout << "Error: " << status.error_code() << ": " << status.error_message() << std::endl;
    }

    return 0;
}