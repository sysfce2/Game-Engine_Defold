package com.dynamo.bob.pipeline;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertArrayEquals;

import org.junit.Test;
import org.junit.runner.JUnitCore;
import org.junit.internal.TextListener;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.IOException;
import java.lang.reflect.Method;

public class TestModelc
{
    @Test
    public void testException() {
        boolean caught = false;

        System.out.println("****************************************************");
        System.out.println("Expected error begin ->");

        try {
            Modelc.TestException("SIGSEGV");
        } catch(Exception e) {
            e.printStackTrace();
            caught = e.getMessage().contains("Exception in Defold JNI code. Signal 11");
        }

        System.out.println("<- Expected error end");
        System.out.println("****************************************************");

        assertTrue(caught);
    }

    // ----------------------------------------------------

    public static void main(String[] args) {
        JUnitCore junit = new JUnitCore();
        junit.addListener(new TextListener(System.out));
        junit.run(TestModelc.class);
    }
}
