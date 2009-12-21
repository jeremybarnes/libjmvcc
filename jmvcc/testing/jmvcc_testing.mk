$(eval $(call test,object_test,jmvcc arch boost_thread-mt,boost))
$(eval $(call test,versioned_test,jmvcc arch boost_thread-mt,boost))
$(eval $(call test,epoch_compression_test,jmvcc arch boost_thread-mt,boost))
$(eval $(call test,garbage_test,jmvcc arch boost_thread-mt,boost))
